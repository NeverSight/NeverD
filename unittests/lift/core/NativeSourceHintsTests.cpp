#include "../../../lib/ir/high/pass/HighDCEDetail.h"
#include "../../../lib/loader/MachO/ImmutableNativeFrame.h"
#include "../../../lib/loader/Swift/SwiftBooleanProjection.h"
#include "../../../lib/pipeline/NativeSourceOutputFrame.h"
#include "../../../lib/pipeline/NativeSourcePreservation.h"
#include "../../../lib/sdk/capi/ObjCNativeDependencies.h"
#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "../../../lib/sdk/capi/ObjCSourceProjection.h"
#include "../../../lib/sdk/capi/SourceSwiftConsumedInputProjection.h"
#include "../../../lib/sdk/capi/SwiftMangledSourceABI.h"
#include "CFunctionParameterCallFixture.h"
#include "ImmutableNativeCallFixture.h"
#include "SwiftConsumedInputFixture.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighSourceFlow.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedSourceParameterUses.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/MachO/DarwinRuntimeCalls.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"
#include "neverd/loader/ObjC/ObjCContextCallEffects.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"
#include "neverd/loader/Swift/SwiftAccessEffects.h"
#include "neverd/loader/Swift/SwiftRuntimeCalls.h"
#include "neverd/loader/Swift/SwiftValueBufferEffects.h"
#include "neverd/pipeline/NativeSourceHints.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>
#include <functional>
#include <iterator>
#include <map>
#include <vector>

using namespace neverd;

TEST(NativeSourceHints, UserPrototypeKeepsExplicitCallSiteAuthority) {
  // An explicit occurrence contract remains authoritative even when the target
  // also has a user prototype and a different inferred callee signature.
  for (const Arch Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    LowFunc Low;
    Low.Entry = 0x1000;
    LowBlock Block;
    Block.Id = 0;
    Block.StartAddr = Low.Entry;
    Block.EndAddr = Low.Entry + 8;
    LowOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = Low.Entry;
    Call.addInput(NdVar::cst(0x2000, 8));
    Block.Ops.push_back(Call);
    LowOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Low.Entry + 4;
    Return.addInput(NdVar::reg(getTargetRegInfo(Architecture).IntReturnReg, 8));
    Block.Ops.push_back(Return);
    Low.Blocks.push_back(Block);

    SourceCallTypeHint Explicit;
    Explicit.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
    Explicit.TargetAddress = 0x2000;
    Explicit.TargetName = "read";
    Explicit.Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
    Explicit.Signature.ReturnType = NdType::makeInt(8, true);
    Explicit.Signature.Parameters = {{"first", NdType::makeInt(8, true)},
                                     {"second", NdType::makeInt(8, true)}};
    std::string Error;
    ASSERT_TRUE(
        assignDarwinScalarSourceABI(Explicit.Signature, Architecture, Error))
        << Error;
    auto Inferred = Explicit.Signature;
    Inferred.Parameters.pop_back();
    ASSERT_TRUE(assignDarwinScalarSourceABI(Inferred, Architecture, Error))
        << Error;
    const std::map<SourceCallOccurrenceKey, SourceCallTypeHint> Sites{
        {{Call.Addr, Call.Seq, Call.Opcode, 0x2000}, Explicit}};
    const std::map<va_t, SourceFunctionTypeHint> Callees{{0x2000, Inferred}};
    const std::set<va_t> UserPrototypes{0x2000};
    LowToMedConverter Converter;
    Converter.setSourceCallHintsEnabled(true);
    Converter.setSourceCallSiteHints(&Sites);
    Converter.setSourceCalleeTypeHints(&Callees);
    Converter.setUserPrototypeCallees(&UserPrototypes);
    const auto Med = Converter.convert(Low, Architecture, BinaryFormat::MachO);
    size_t Calls = 0;
    for (const auto &B : Med.Blocks)
      for (const auto &Op : B.Ops)
        if (Op.Opcode == NdOp::CALL) {
          ++Calls;
          ASSERT_TRUE(Op.SourceCallHint);
          EXPECT_TRUE(equalSourceABIs(Op.SourceCallHint->Signature,
                                      Explicit.Signature));
          EXPECT_EQ(Op.NumInputs, 3u);
        }
    EXPECT_EQ(Calls, 1u);
  }
}

namespace {
struct HFAImportVeneerFixture {
  static constexpr va_t Entry = 0x1000, Slot = 0x2080;
  BinaryImage Image;
  PipelineOptions Options;
  llvm::LLVMContext Context;
  PipelineResult Result;

  HFAImportVeneerFixture(const char *Name, const char *Provider) {
    Image.Arch = Arch::AArch64;
    Image.Format = BinaryFormat::MachO;
    Image.Bits = Bitness::Bits64;
    Image.DynInfo.NeededLibs = {Provider};
    for (unsigned I = 0; I != 2; ++I) {
      Segment S;
      S.VA = Entry + I * 0x1000;
      S.FileOff = I * 0x100;
      S.Size = S.FileSz = 0x100;
      S.Flags = SegmentFlags::Readable |
                (I ? SegmentFlags::None : SegmentFlags::Executable);
      S.Data.resize(0x100);
      if (!I) {
        const uint32_t Words[] = {0xb0000010, 0xf9404210, 0xd61f0200};
        for (unsigned J = 0; J != 3; ++J)
          llvm::support::endian::write32le(S.Data.data() + 4 * J, Words[J]);
      }
      Section Sec;
      Sec.VA = S.VA;
      Sec.FileOff = S.FileOff;
      Sec.Size = Sec.FileSz = S.FileSz;
      Sec.Flags = S.Flags;
      Sec.Type = I ? 0 : llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
      Image.Sections.push_back(Sec);
      Image.Segments.push_back(std::move(S));
    }
    Image.ImportPtrSlots[Slot] = Name;
    EXPECT_TRUE(Image.recordDyldBindSlot(Slot, Name, 0, Provider, false));
    Image.Symbols.push_back({"anonymous_veneer", Entry, 12, true});
    Options.OnlyFunctionEntries = {Entry};
    Options.EmitDumpOutput = false;
    Result = Pipeline().run(Image, Context, Options);
  }

  size_t infer() {
    std::map<va_t, std::string> Diagnostics;
    return sdk::inferObjCNativeDependencies(Image, Result, Options, Diagnostics,
                                            {Entry});
  }
};

struct SwiftErrorDeclarationFixture {
  static constexpr va_t Entry = 0x1000;
  BinaryImage Image;
  PipelineOptions Options;
  llvm::LLVMContext Context;
  PipelineResult Result;

  explicit SwiftErrorDeclarationFixture(Arch Architecture,
                                        bool RepeatedDeclaration = false) {
    Image.Arch = Architecture;
    Image.Format = BinaryFormat::MachO;
    Image.Bits = Bitness::Bits64;
    Segment Code;
    Code.VA = Entry;
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    if (Architecture == Arch::AArch64) {
      // Return swiftself, replace swifterror with the second ordinary input.
      const uint32_t Words[] = {0xaa1403e0, 0xaa0103f5, 0xd65f03c0};
      Code.Data.resize(sizeof(Words));
      for (unsigned I = 0; I != std::size(Words); ++I)
        llvm::support::endian::write32le(Code.Data.data() + 4 * I, Words[I]);
    } else {
      // mov rax, r13; mov r12, rsi; ret
      Code.Data = {0x4c, 0x89, 0xe8, 0x49, 0x89, 0xf4, 0xc3};
    }
    Code.Size = Code.FileSz = Code.Data.size();
    Image.Symbols.push_back({"_$sSo19NSRegularExpressionC7pattern7optionsABSS_"
                             "So0aB7OptionsVtKcfcTO",
                             Entry, Code.Size, true});
    if (RepeatedDeclaration)
      Image.Symbols.push_back(Image.Symbols.front());
    Image.Segments.push_back(std::move(Code));
    SourceFunctionTypeHint Scalar;
    Scalar.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Scalar.ReturnType = NdType::makePtr(NdType::makeVoid());
    Scalar.Parameters = {{"bits", NdType::makeInt(8, false)},
                         {"object", Scalar.ReturnType},
                         {"options", NdType::makeInt(8, false)}};
    std::string Error;
    EXPECT_TRUE(assignDarwinScalarSourceABI(Scalar, Architecture, Error))
        << Error;
    Options.SourceTypeHints = {{Entry, Scalar}};
    Options.OnlyFunctionEntries = {Entry};
    Options.EmitDumpOutput = false;
    Result = Pipeline().run(Image, Context, Options);
  }

  size_t infer() {
    std::map<va_t, std::string> Diagnostics;
    return sdk::inferObjCNativeDependencies(Image, Result, Options, Diagnostics,
                                            {Entry});
  }
};

struct SwiftErrorCallerFixture : SwiftErrorDeclarationFixture {
  static constexpr va_t Caller = 0x1080;
  explicit SwiftErrorCallerFixture(Arch Architecture)
      : SwiftErrorDeclarationFixture(Architecture) {
    const auto Signature =
        sdk::swiftMangledRegularExpressionInitializerSourceABI(Image, Entry);
    EXPECT_TRUE(Signature);
    if (!Signature)
      return;
    Options.SourceTypeHints = {{Entry, *Signature}};
    auto &Code = Image.Segments.front();
    size_t Bytes = 0;
    if (Architecture == Arch::AArch64) {
      // Save swiftself/swifterror and the return address; call once and branch
      // on the new error. Both exits restore the caller's original state.
      const uint32_t Words[] = {0xa9be57f4, 0xa9017bfd, 0x910003fd, 0xaa0003f4,
                                0xaa1f03f5, 0x97ffffdb, 0xb5000055, 0x14000002,
                                0xd28002e0, 0xa9417bfd, 0xa8c257f4, 0xd65f03c0};
      Bytes = sizeof(Words);
      Code.Data.resize(Caller - Entry + Bytes);
      for (unsigned I = 0; I != std::size(Words); ++I)
        llvm::support::endian::write32le(
            Code.Data.data() + Caller - Entry + 4 * I, Words[I]);
    } else {
      // push r12/r13; align rsp; set context and zero error; call the entry;
      // test the returned error, choose 23 on failure, then restore all state.
      // The assembled relative relocations are resolved to the local labels.
      const uint8_t CodeBytes[] = {
          0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xec, 0x08, 0x49, 0x89,
          0xfd, 0x45, 0x31, 0xe4, 0xe8, 0x6d, 0xff, 0xff, 0xff, 0x4d,
          0x85, 0xe4, 0x0f, 0x85, 0x05, 0x00, 0x00, 0x00, 0xe9, 0x07,
          0x00, 0x00, 0x00, 0x48, 0xc7, 0xc0, 0x17, 0x00, 0x00, 0x00,
          0x48, 0x83, 0xc4, 0x08, 0x41, 0x5d, 0x41, 0x5c, 0xc3};
      Bytes = sizeof(CodeBytes);
      Code.Data.resize(Caller - Entry + Bytes);
      std::copy(std::begin(CodeBytes), std::end(CodeBytes),
                Code.Data.begin() + Caller - Entry);
    }
    Code.Size = Code.FileSz = Code.Data.size();
    Section Text;
    Text.Name = "__text";
    Text.VA = Code.VA;
    Text.Size = Text.FileSz = Code.Size;
    Text.Flags = Code.Flags;
    Text.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    Image.Sections.push_back(std::move(Text));
    Image.Symbols.push_back(
        {"_native_swift_error_caller", Caller, Bytes, true});
    Options.OnlyFunctionEntries = {Entry, Caller};
    Result = Pipeline().run(Image, Context, Options);
  }

  const MedFunc &callerMed() const {
    return *std::find_if(Result.MedFuncs.begin(), Result.MedFuncs.end(),
                         [](const MedFunc &F) { return F.Entry == Caller; });
  }
  const HighFunc &callerHigh() const {
    return *std::find_if(Result.HighFuncs.begin(), Result.HighFuncs.end(),
                         [](const HighFunc &F) { return F.Entry == Caller; });
  }
  const LowFunc &callerLow() const {
    return *std::find_if(Result.LowFuncs.begin(), Result.LowFuncs.end(),
                         [](const LowFunc &F) { return F.Entry == Caller; });
  }
  const PipelineFunctionAudit &callerAudit() const {
    return *std::find_if(Result.FunctionAudits.begin(),
                         Result.FunctionAudits.end(),
                         [](const auto &A) { return A.Entry == Caller; });
  }
};

SourceFunctionTypeHint nativeOutputSignature() {
  SourceFunctionTypeHint Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Signature.ReturnType = NdType::makeVoid();
  Signature.Parameters = {{"first", NdType::makeFloat(8)},
                          {"second", NdType::makeFloat(8)},
                          {"out", NdType::makePtr(NdType::makeVoid())}};
  std::string Error;
  EXPECT_TRUE(assignDarwinScalarSourceABI(Signature, Arch::AArch64, Error))
      << Error;
  Signature.Parameters.back().Location.RegisterOffset = a64reg::X8;
  return Signature;
}

struct NativeOutputFixture : HFAImportVeneerFixture {
  static constexpr va_t Caller = 0x1040, Helper = 0x1080;
  static constexpr const char *Provider =
      "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics";
  NativeOutputFixture(bool Complete = true)
      : HFAImportVeneerFixture("_CGAffineTransformInvert", Provider) {
    // Assembled ARM64: the helper writes three d0/d1 pairs through x8.
    // Its caller lends 48 private bytes, then consumes them through the SDK
    // affine bridge and forwards its explicit entry result address.
    const uint32_t CallerWords[] = {
        0xd10143ff, 0xa90353f3, 0xa9047bfd, 0xaa0803f3, 0x910003e8,
        0x9400000b, 0x910003e0, 0xaa1303e8, 0x97ffffe8, 0xa9447bfd,
        0xa94353f3, 0x910143ff, 0xd65f03c0};
    const uint32_t HelperWords[] = {0x6d000500, 0x6d010500, 0x6d020500,
                                    0xd65f03c0};
    for (unsigned I = 0; I != std::size(CallerWords); ++I)
      llvm::support::endian::write32le(Image.Segments[0].Data.data() + Caller -
                                           Entry + 4 * I,
                                       CallerWords[I]);
    for (unsigned I = 0; I != std::size(HelperWords); ++I)
      llvm::support::endian::write32le(Image.Segments[0].Data.data() + Helper -
                                           Entry + 4 * I,
                                       HelperWords[I]);
    if (!Complete)
      llvm::support::endian::write32le(
          Image.Segments[0].Data.data() + Helper - Entry + 8, 0xd65f03c0);
    Image.Symbols.push_back(
        {"_native_output_consumer", Caller, sizeof(CallerWords), true});
    Image.Symbols.push_back({"_native_output_producer", Helper,
                             Complete ? sizeof(HelperWords) : 12, true});
    const auto Signature = nativeOutputSignature();
    Options.SourceTypeHints = {{Caller, Signature}, {Helper, Signature}};
    Options.OnlyFunctionEntries = {Caller, Helper};
    Result = Pipeline().run(Image, Context, Options);
  }
  SourceCallTypeHint binding() const {
    SourceCallTypeHint B;
    B.CallKind = SourceCallTypeHint::Kind::Native;
    B.TargetAddress = Helper;
    B.Signature = Options.SourceTypeHints.at(Helper);
    return B;
  }
};

struct NativeSDKOutputFixture : HFAImportVeneerFixture {
  static constexpr va_t Helper = 0x1080;
  NativeSDKOutputFixture()
      : HFAImportVeneerFixture("_CGAffineTransformMakeScale",
                               NativeOutputFixture::Provider) {
    // Assembled b 0x1000 at 0x1080: the current SDK contract completely writes
    // its 48-byte indirect result through the explicit entry x8 parameter.
    llvm::support::endian::write32le(
        Image.Segments[0].Data.data() + Helper - Entry, 0x17ffffe0);
    Image.Symbols.push_back({"_native_sdk_output", Helper, 4, true});
    Options.OnlyFunctionEntries = {Helper};
    Options.SourceTypeHints = {{Helper, nativeOutputSignature()}};
    Result = Pipeline().run(Image, Context, Options);
  }
  SourceCallTypeHint binding() const {
    SourceCallTypeHint B;
    B.CallKind = SourceCallTypeHint::Kind::Native;
    B.TargetAddress = Helper;
    B.Signature = Options.SourceTypeHints.at(Helper);
    return B;
  }
};
} // namespace

TEST(NativeSourceHints, SwiftErrorDeclarationRefinesTheObservedScalarABI) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Repeated : {false, true}) {
      SCOPED_TRACE(int(Architecture));
      SCOPED_TRACE(Repeated);
      SwiftErrorDeclarationFixture F(Architecture, Repeated);
      ASSERT_TRUE(F.Result.Success) << F.Result.Error;
      ASSERT_EQ(F.Result.HighFuncs.size(), 1U);
      ASSERT_EQ(F.Result.FunctionAudits.size(), 1U);
      ASSERT_EQ(F.Options.SourceTypeHints.at(F.Entry).Parameters.size(), 3U);
      ASSERT_EQ(F.infer(), 1U);
      const auto &Hint = F.Options.SourceTypeHints.at(F.Entry);
      EXPECT_EQ(Hint.Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
      ASSERT_EQ(Hint.Parameters.size(), 5U);
      EXPECT_TRUE(sourceABIErrorResult(Hint));
      EXPECT_EQ(Hint.Parameters[3].TheRole,
                SourceParameterTypeHint::Role::SwiftContext);
      auto Relifted = Pipeline().run(F.Image, F.Context, F.Options);
      ASSERT_TRUE(Relifted.Success) << Relifted.Error;
      ASSERT_EQ(Relifted.HighFuncs.size(), 1U);
      ASSERT_EQ(Relifted.FunctionAudits.size(), 1U);
      const auto Bound =
          sdk::bindObjCSourceReferences(Relifted.HighFuncs[0], F.Image);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      EXPECT_TRUE(isSwiftErrorEntryProjected(Bound.Function, Architecture));
      EXPECT_TRUE(sdk::sourceBodyLimitation(
                      Bound.Function, Hint, &Relifted.FunctionAudits[0],
                      [](const HighExpr &) { return false; })
                      .empty());
      auto Cleaned = Bound.Function;
      eliminateUnusedValues(Cleaned);
      EXPECT_TRUE(isSwiftErrorEntryProjected(Cleaned, Architecture));
      F.Result = std::move(Relifted);
      EXPECT_EQ(F.infer(), 0U);
    }
}

TEST(NativeSourceHints, SwiftErrorCallsRequireCurrentDirectNativeProjection) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    SwiftErrorDeclarationFixture F(Architecture);
    ASSERT_EQ(F.infer(), 1U);
    auto Relifted = Pipeline().run(F.Image, F.Context, F.Options);
    ASSERT_TRUE(Relifted.Success) << Relifted.Error;
    ASSERT_EQ(Relifted.HighFuncs.size(), 1U);
    const auto &Signature = F.Options.SourceTypeHints.at(F.Entry);
    SourceCallTypeHint Binding;
    Binding.CallKind = SourceCallTypeHint::Kind::Native;
    Binding.TargetAddress = F.Entry;
    Binding.Signature = Signature;
    std::vector<ExprPtr> Arguments;
    for (const auto &Parameter : Signature.Parameters) {
      auto Argument = HighExpr::makeConst(0, 8);
      Argument->Type =
          Parameter.TheRole == SourceParameterTypeHint::Role::SwiftErrorResult
              ? sourceABIErrorResult(Signature)->Type
              : Parameter.Type;
      Arguments.push_back(std::move(Argument));
    }
    auto Call = HighExpr::makeCall("error_entry", F.Entry, Arguments);
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Binding);
    Call->Type = swiftErrorCallResultType(Signature);
    std::map<va_t, const HighFunc *> Functions{
        {F.Entry, &Relifted.HighFuncs[0]}};
    ASSERT_TRUE(sdk::objcSourceCallBound(*Call, F.Image, Functions));
    for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
      HighExpr Bad = *Call;
      auto BadBinding = std::make_shared<SourceCallTypeHint>(Binding);
      Bad.SourceCallHint = BadBinding;
      auto Callee = Relifted.HighFuncs[0];
      auto CurrentFunctions = Functions;
      CurrentFunctions[F.Entry] = &Callee;
      switch (Mutation) {
      case 0:
        CurrentFunctions.clear();
        break;
      case 1:
        CurrentFunctions[F.Entry] = nullptr;
        break;
      case 2:
        ++Callee.Entry;
        break;
      case 3:
        Callee.SwiftErrorEntry.reset();
        break;
      case 4:
        Callee.Body.erase(Callee.Body.begin());
        break;
      case 5:
        ++Bad.CallAddr;
        break;
      case 6:
        Bad.IsIndirectCall = true;
        break;
      case 7:
        Bad.Type = Signature.ReturnType;
        break;
      case 8:
        Bad.Operands.pop_back();
        break;
      case 9:
        BadBinding->DoesNotReturn = true;
        break;
      case 10:
        BadBinding->WeakImport = true;
        break;
      }
      EXPECT_FALSE(sdk::objcSourceCallBound(Bad, F.Image, CurrentFunctions))
          << int(Architecture) << ": " << Mutation;
    }
  }
}

TEST(NativeSourceHints, SwiftErrorDeclarationKeepsExplicitSourceAuthority) {
  for (unsigned Authority = 0; Authority != 3; ++Authority) {
    SwiftErrorDeclarationFixture F(Arch::AArch64);
    ASSERT_TRUE(F.Result.Success) << F.Result.Error;
    ASSERT_EQ(F.Result.MedFuncs.size(), 1U);
    ASSERT_EQ(F.Result.HighFuncs.size(), 1U);
    auto Explicit = F.Options.SourceTypeHints.at(F.Entry);
    Explicit.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
    if (Authority == 0)
      F.Options.SourceTypeHints[F.Entry] = Explicit;
    if (Authority == 1) {
      F.Options.SourceTypeHints.clear();
      F.Result.MedFuncs[0].SourceTypeHint = Explicit;
    }
    if (Authority == 2) {
      F.Options.SourceTypeHints.clear();
      F.Result.HighFuncs[0].SourceTypeHint = Explicit;
    }
    EXPECT_EQ(F.infer(), 0U) << Authority;
    if (Authority == 0)
      EXPECT_TRUE(
          equalSourceABIs(F.Options.SourceTypeHints.at(F.Entry), Explicit));
    else
      EXPECT_TRUE(F.Options.SourceTypeHints.empty());
  }
}

TEST(NativeSourceHints, SwiftErrorDeclarationRequiresCompleteCurrentAudit) {
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SwiftErrorDeclarationFixture F(Arch::AArch64);
    ASSERT_TRUE(F.Result.Success) << F.Result.Error;
    ASSERT_EQ(F.Result.FunctionAudits.size(), 1U);
    const auto Original = F.Options.SourceTypeHints.at(F.Entry);
    auto &Audit = F.Result.FunctionAudits[0];
    switch (Mutation) {
    case 0:
      Audit.Disposition = PipelineFunctionDisposition::Candidate;
      break;
    case 1:
      Audit.HasLowIR = false;
      break;
    case 2:
      Audit.MedIRVerified = false;
      break;
    case 3:
      --Audit.LiftedInstructions;
      break;
    case 4:
      Audit.DecodeFailures.push_back(F.Entry);
      break;
    case 5:
      Audit.TruncatedPaths.push_back(F.Entry);
      break;
    case 6:
      ++Audit.Entry;
      break;
    case 7:
      Audit.HasMedIR = false;
      break;
    }
    EXPECT_EQ(F.infer(), 0U) << Mutation;
    EXPECT_TRUE(
        equalSourceABIs(F.Options.SourceTypeHints.at(F.Entry), Original));
  }
}

TEST(NativeSourceHints, SwiftErrorCallResultsSurviveAutomaticNativeInference) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(int(Architecture));
    SwiftErrorCallerFixture F(Architecture);
    ASSERT_TRUE(F.Result.Success) << F.Result.Error;
    ASSERT_EQ(F.Result.LowFuncs.size(), 2U);
    ASSERT_EQ(F.Result.MedFuncs.size(), 2U);
    ASSERT_EQ(F.Result.HighFuncs.size(), 2U);
    ASSERT_EQ(F.Result.FunctionAudits.size(), 2U);
    ASSERT_FALSE(F.callerMed().SourceTypeHint);
    const auto Contracts = nativeSourceCalleeContracts(F.Image, F.Result);
    std::string Diagnostic;
    const auto Hint = inferNativeSourceTypeHint(
        F.Image, F.callerMed(), F.callerHigh(), F.callerAudit(), Diagnostic,
        &F.callerLow(), false, &Contracts);
    ASSERT_TRUE(Hint) << Diagnostic;
    EXPECT_NE(Hint->ReturnType->Kind, NdTypeKind::Void);
    EXPECT_EQ(Hint->ReturnLocation.RegisterOffset,
              getTargetRegInfo(Architecture).IntReturnRegs.front());
    EXPECT_EQ(Hint->ReturnLocation.ValueBytes, 8U);
    F.Options.SourceTypeHints.emplace(F.Caller, *Hint);
    auto Relifted = Pipeline().run(F.Image, F.Context, F.Options);
    ASSERT_TRUE(Relifted.Success) << Relifted.Error;
    std::map<va_t, const HighFunc *> Functions;
    for (const auto &Function : Relifted.HighFuncs)
      Functions.emplace(Function.Entry, &Function);
    const auto Bound =
        sdk::bindObjCSourceReferences(*Functions.at(F.Caller), F.Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    const auto Allowed = [&](const HighExpr &Call) {
      return sdk::objcSourceCallBound(Call, F.Image, Functions);
    };
    EXPECT_TRUE(sdk::sourceBodyLimitation(Bound.Function, *Hint,
                                          &F.callerAudit(), Allowed)
                    .empty());
  }
}

TEST(NativeSourceHints, SwiftErrorCallResultsRequireCurrentPairedEvidence) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    SwiftErrorCallerFixture F(Architecture);
    ASSERT_TRUE(F.Result.Success) << F.Result.Error;
    ASSERT_EQ(F.Result.MedFuncs.size(), 2U);
    const auto Original = nativeSourceCalleeContracts(F.Image, F.Result);
    for (unsigned Mutation = 0; Mutation != 25; ++Mutation) {
      SCOPED_TRACE(int(Architecture));
      SCOPED_TRACE(Mutation);
      auto Contracts = Original;
      auto Med = F.callerMed();
      auto Low = F.callerLow();
      auto &C = Contracts.CurrentCallees.at(F.Entry);
      auto CalleeLow = *C.Low;
      auto CalleeMed = *C.Med;
      auto CalleeHigh = *C.High;
      auto CalleeAudit = *C.Audit;
      auto Signature = *C.Signature;
      C = {&CalleeLow, &Signature, &CalleeAudit, &CalleeMed, &CalleeHigh};
      auto Block = std::find_if(
          Med.Blocks.begin(), Med.Blocks.end(), [](const MedBlock &B) {
            return std::any_of(B.Ops.begin(), B.Ops.end(), [](const MedOp &O) {
              return O.Opcode == NdOp::CALL && O.SourceCallHint &&
                     O.SourceCallHint->TargetAddress ==
                         SwiftErrorCallerFixture::Entry;
            });
          });
      ASSERT_NE(Block, Med.Blocks.end());
      auto Call = std::find_if(
          Block->Ops.begin(), Block->Ops.end(), [](const MedOp &O) {
            return O.Opcode == NdOp::CALL && O.SourceCallHint &&
                   O.SourceCallHint->TargetAddress ==
                       SwiftErrorCallerFixture::Entry;
          });
      ASSERT_GE(std::distance(Call, Block->Ops.end()), 3);
      switch (Mutation) {
      case 0:
        Contracts.CurrentCallees.clear();
        break;
      case 1:
        Contracts.SourceImage = nullptr;
        break;
      case 2:
        C.Signature = nullptr;
        break;
      case 3:
        C.Low = nullptr;
        break;
      case 4:
        C.Med = nullptr;
        break;
      case 5:
        C.High = nullptr;
        break;
      case 6:
        C.Audit = nullptr;
        break;
      case 7:
        ++CalleeAudit.DecodedInstructions;
        break;
      case 8:
        CalleeLow.Blocks[0].Ops.back().Addr++;
        break;
      case 9:
        CalleeHigh.Body.erase(CalleeHigh.Body.begin());
        break;
      case 10:
        Signature.Parameters.back().Location.RegisterOffset += 8;
        break;
      case 11:
        Call->SourceCallHint.reset();
        break;
      case 12:
        --Call->NumInputs;
        break;
      case 13:
        Call->Output.Size = 8;
        break;
      case 14:
        (Call + 2)->Output.RegOff += 8;
        break;
      case 15:
        (Call + 1)->Inputs[0].Id++;
        break;
      case 16:
        (Call + 2)->Inputs[1] = MedVar::makeConst(4, 4);
        break;
      case 17:
        (Call + 2)->Output.Size = 4;
        break;
      case 18:
        Call->OriginSeq++;
        break;
      case 19:
        for (auto &B : Low.Blocks)
          for (auto &O : B.Ops)
            if (O.Opcode == NdOp::CALL && O.NumInputs &&
                O.Inputs[0].isConst() && O.Inputs[0].Offset == F.Entry)
              O.Inputs[0].Offset += 4;
        break;
      case 20:
        Block->Ops.erase(Call + 2);
        break;
      case 21:
        CalleeMed.SourceParametersBound = false;
        break;
      case 22:
        CalleeHigh.SourceTypeHint.reset();
        break;
      case 23:
        CalleeAudit.Disposition = PipelineFunctionDisposition::Candidate;
        break;
      case 24:
        ++(Call + 1)->Addr;
        break;
      }
      std::string Diagnostic;
      EXPECT_FALSE(inferNativeSourceTypeHint(F.Image, Med, F.callerHigh(),
                                             F.callerAudit(), Diagnostic, &Low,
                                             false, &Contracts))
          << Diagnostic;
    }
  }
}

TEST(NativeSourceHints,
     CompleteNativeOutputReplaysMachineAndFeedsSDKFrameInput) {
  for (bool Complete : {true, false}) {
    SCOPED_TRACE(Complete);
    NativeOutputFixture F(Complete);
    ASSERT_TRUE(F.Result.Success) << F.Result.Error;
    const auto Contracts = nativeSourceCalleeContracts(F.Image, F.Result);
    const auto Effects =
        detail::nativeOutputFrameEffects(F.Image, F.binding(), &Contracts);
    ASSERT_TRUE(Effects);
    EXPECT_EQ(Effects->WritableFrameParameters.at(2), Complete ? 48U : 32U);
    EXPECT_EQ(Effects->InitializesFrameParameters, std::set<size_t>{2});
    const auto Current = Contracts.CurrentCallees.at(F.Caller);
    ASSERT_TRUE(Current.Low && Current.Med && Current.Audit &&
                Current.Signature);
    const auto H =
        std::find_if(F.Result.HighFuncs.begin(), F.Result.HighFuncs.end(),
                     [](const HighFunc &HF) {
                       return HF.Entry == NativeOutputFixture::Caller;
                     });
    ASSERT_NE(H, F.Result.HighFuncs.end());
    std::map<va_t, const HighFunc *> Functions;
    for (const auto &HF : F.Result.HighFuncs)
      Functions.emplace(HF.Entry, &HF);
    const auto Bound = sdk::bindObjCSourceReferences(*H, F.Image);
    const auto Allowed = [&](const HighExpr &Call) {
      return sdk::objcSourceCallBound(Call, F.Image, Functions);
    };
    // An explicit entry declaration alone does not authenticate native frame
    // effects. Exercise the unbound caller's actual inference boundary below.
    auto InferenceOptions = F.Options;
    InferenceOptions.SourceTypeHints.erase(F.Caller);
    auto Unbound = Pipeline().run(F.Image, F.Context, InferenceOptions);
    ASSERT_TRUE(Unbound.Success) << Unbound.Error;
    std::map<va_t, std::string> Diagnostics;
    sdk::inferObjCNativeDependencies(F.Image, Unbound, InferenceOptions,
                                     Diagnostics, {F.Caller});
    EXPECT_EQ(InferenceOptions.SourceTypeHints.count(F.Caller),
              Complete ? 1U : 0U)
        << Diagnostics[F.Caller];
    if (Complete) {
      EXPECT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      EXPECT_TRUE(sdk::sourceBodyLimitation(Bound.Function, *Current.Signature,
                                            Current.Audit, Allowed)
                      .empty());
      auto Relifted = Pipeline().run(F.Image, F.Context, F.Options);
      ASSERT_TRUE(Relifted.Success) << Relifted.Error;
      const auto Fresh = nativeSourceCalleeContracts(F.Image, Relifted);
      const auto Replayed =
          detail::nativeOutputFrameEffects(F.Image, F.binding(), &Fresh);
      ASSERT_TRUE(Replayed);
      EXPECT_EQ(*Effects, *Replayed);
    }
  }
}

TEST(NativeSourceHints, CompleteNativeOutputRejectsStaleCurrentEvidence) {
  for (unsigned Mutation = 0; Mutation != 17; ++Mutation) {
    SCOPED_TRACE(Mutation);
    NativeOutputFixture F;
    ASSERT_TRUE(F.Result.Success) << F.Result.Error;
    auto Contracts = nativeSourceCalleeContracts(F.Image, F.Result);
    auto Binding = F.binding();
    ASSERT_TRUE(detail::nativeOutputFrameEffects(F.Image, Binding, &Contracts));
    auto &C = Contracts.CurrentCallees.at(F.Helper);
    auto Low = *C.Low;
    auto Med = *C.Med;
    auto Audit = *C.Audit;
    auto Signature = *C.Signature;
    C.Low = &Low;
    C.Med = &Med;
    C.Audit = &Audit;
    C.Signature = &Signature;
    if (Mutation == 0)
      Contracts.SourceImage = nullptr;
    if (Mutation == 1)
      C.Low = nullptr;
    if (Mutation == 2)
      C.Med = nullptr;
    if (Mutation == 3)
      C.Audit = nullptr;
    if (Mutation == 4)
      Med.SourceParametersBound = false;
    if (Mutation == 5)
      Audit.MedIRVerified = false;
    if (Mutation == 6)
      ++Audit.DecodedInstructions;
    if (Mutation == 7)
      Low.Blocks[0].Ops[0].Opcode = NdOp::INT_XOR;
    if (Mutation == 8) {
      F.Image.Segments[0].Data[F.Helper - F.Entry] ^= 1;
    }
    if (Mutation == 9)
      F.Image.Segments[0].Flags =
          F.Image.Segments[0].Flags | SegmentFlags::Writable;
    if (Mutation == 10)
      F.Image.Sections.push_back(F.Image.Sections[0]);
    if (Mutation == 11)
      F.Image.CodePtrRelocSlots.insert(F.Helper);
    if (Mutation == 12)
      Binding.Signature.Parameters[2].Location.RegisterOffset = a64reg::X0;
    if (Mutation == 13)
      Binding.Signature.Origin =
          SourceFunctionTypeHint::OriginKind::ExplicitSource;
    if (Mutation == 14)
      Signature.ReturnType = NdType::makeInt(8);
    if (Mutation == 15)
      Binding.TargetAddress = F.Caller;
    if (Mutation == 16)
      Binding.WeakImport = true;
    EXPECT_FALSE(
        detail::nativeOutputFrameEffects(F.Image, Binding, &Contracts));
  }
}

TEST(NativeSourceHints, CompleteNativeOutputAuthenticatesCurrentSDKTailCall) {
  NativeSDKOutputFixture F;
  ASSERT_TRUE(F.Result.Success) << F.Result.Error;
  const auto Contracts = nativeSourceCalleeContracts(F.Image, F.Result);
  const auto Effects =
      detail::nativeOutputFrameEffects(F.Image, F.binding(), &Contracts);
  ASSERT_TRUE(Effects);
  EXPECT_EQ(Effects->WritableFrameParameters.at(2), 48U);
  EXPECT_EQ(Effects->InitializesFrameParameters, std::set<size_t>{2});

  for (unsigned Mutation = 0; Mutation != 18; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = F.Image;
    auto Current = Contracts;
    Current.SourceImage = &Image;
    auto &C = Current.CurrentCallees.at(F.Helper);
    auto Med = *C.Med;
    C.Med = &Med;
    MedOp *Call = nullptr;
    for (auto &B : Med.Blocks)
      for (auto &Op : B.Ops)
        if (Op.Opcode == NdOp::CALL)
          Call = &Op;
    ASSERT_TRUE(Call);
    ASSERT_TRUE(Call->SourceCallHint);
    auto Hint = *Call->SourceCallHint;
    switch (Mutation) {
    case 0:
      Image.DyldBindSlots.at(F.Slot).Module = "/tmp/CoreGraphics";
      break;
    case 1:
      Image.DyldBindSlots.at(F.Slot).WeakImport = true;
      break;
    case 2:
      Image.DynInfo.NeededLibs.clear();
      break;
    case 3:
      Image.ImportPtrSlots.at(F.Slot) = "_CGAffineTransformInvert";
      break;
    case 4:
      Image.Segments[0].Data[0] ^= 1;
      break;
    case 5:
      ++Call->OriginSeq;
      break;
    case 6:
      Call->CallSiteId = 0;
      break;
    case 7:
      --Call->Inputs[1].Size;
      break;
    case 8:
      --Call->NumInputs;
      break;
    case 9:
      --Call->Output.Size;
      break;
    case 10:
      Hint.TargetName = "_CGAffineTransformMakeTranslation";
      break;
    case 11:
      Hint.TargetAddress += 4;
      break;
    case 12:
      Hint.Signature.ReturnLocation.RegisterOffset = a64reg::X0;
      break;
    case 13:
      Call->DoesNotReturn = true;
      break;
    case 14:
      Call->PreservesCallerSaved = true;
      break;
    case 15:
      Hint.Signature.Origin =
          SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      break;
    case 16:
      --Call->Inputs[0].Size;
      break;
    case 17:
      Image.DyldBindSlots.at(F.Slot).Addend = 8;
      break;
    }
    Call->SourceCallHint = std::make_shared<const SourceCallTypeHint>(Hint);
    EXPECT_FALSE(
        detail::nativeOutputFrameEffects(Image, F.binding(), &Current));
  }
}

TEST(NativeSourceHints, PreservedFPPrefixesRetainAllEntryInputsAfterSDKCalls) {
  HFAImportVeneerFixture F("_CGRectStandardize", NativeOutputFixture::Provider);
  constexpr va_t Caller = 0x1040, Scale = 0x1010, ScaleSlot = 0x2088;
  // Assembled ARM64: keep the four incoming rectangle lanes in v8-v11
  // across a MakeScale call, then consume only their preserved low D views.
  const uint32_t ScaleWords[] = {0xb0000010, 0xf9404610, 0xd61f0200};
  const uint32_t CallerWords[] = {
      0xd10183ff, 0x6d0327e8, 0x6d042fea, 0xa9057bfd, 0x4ea01c08, 0x4ea11c29,
      0x4ea21c4a, 0x4ea31c6b, 0x910003e8, 0x1e6e1000, 0x1e601001, 0x97ffffe9,
      0x4ea81d00, 0x4ea91d21, 0x4eaa1d42, 0x4eab1d63, 0x97ffffe0, 0xa9457bfd,
      0x6d442fea, 0x6d4327e8, 0x910183ff, 0xd65f03c0};
  for (unsigned I = 0; I < std::size(ScaleWords); ++I)
    llvm::support::endian::write32le(F.Image.Segments[0].Data.data() + Scale -
                                         F.Entry + I * 4,
                                     ScaleWords[I]);
  for (unsigned I = 0; I < std::size(CallerWords); ++I)
    llvm::support::endian::write32le(F.Image.Segments[0].Data.data() + Caller -
                                         F.Entry + I * 4,
                                     CallerWords[I]);
  F.Image.ImportPtrSlots[ScaleSlot] = "_CGAffineTransformMakeScale";
  ASSERT_TRUE(F.Image.recordDyldBindSlot(ScaleSlot,
                                         "_CGAffineTransformMakeScale", 0,
                                         NativeOutputFixture::Provider, false));
  F.Image.Symbols.push_back({"_scale_veneer", Scale, sizeof(ScaleWords), true});
  F.Image.Symbols.push_back(
      {"_preserved_rect_input", Caller, sizeof(CallerWords), true});
  F.Options.OnlyFunctionEntries = {Caller};
  F.Result = Pipeline().run(F.Image, F.Context, F.Options);
  ASSERT_TRUE(F.Result.Success) << F.Result.Error;
  ASSERT_EQ(F.Result.MedFuncs.size(), 1U);
  ASSERT_EQ(F.Result.MedFuncs[0].Params.size(), 4U);
  std::map<va_t, std::string> Diagnostics;
  ASSERT_EQ(sdk::inferObjCNativeDependencies(F.Image, F.Result, F.Options,
                                             Diagnostics, {Caller}),
            1U)
      << Diagnostics[Caller];
  const auto &Hint = F.Options.SourceTypeHints.at(Caller);
  ASSERT_EQ(Hint.Parameters.size(), 4U);
  for (size_t I = 0; I != 4; ++I) {
    EXPECT_EQ(Hint.Parameters[I].Type->Kind, NdTypeKind::Float);
    EXPECT_EQ(Hint.Parameters[I].Type->Size, 8U);
    EXPECT_EQ(Hint.Parameters[I].Location.RegisterOffset, a64reg::V0 + I * 16);
    EXPECT_EQ(Hint.Parameters[I].Location.ValueBytes, 8U);
  }
  auto Fresh = Pipeline().run(F.Image, F.Context, F.Options);
  ASSERT_TRUE(Fresh.Success) << Fresh.Error;
  ASSERT_EQ(Fresh.HighFuncs.size(), 1U);
  EXPECT_TRUE(equalSourceABIs(*Fresh.MedFuncs[0].SourceTypeHint, Hint));
  const auto Bound = sdk::bindObjCSourceReferences(Fresh.HighFuncs[0], F.Image);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  std::map<va_t, const HighFunc *> Functions{{Caller, &Fresh.HighFuncs[0]}};
  const auto Allowed = [&](const HighExpr &Call) {
    return sdk::objcSourceCallBound(Call, F.Image, Functions);
  };
  EXPECT_TRUE(sdk::sourceBodyLimitation(Bound.Function, Hint,
                                        &Fresh.FunctionAudits[0], Allowed)
                  .empty());
}

TEST(NativeSourceHints, HFAImportVeneerRetainsCompleteDeclaredResult) {
  struct Case {
    const char *Name, *Provider;
    size_t Components;
  };
  for (const auto &C : {
           Case{
               "_CGRectStandardize",
               "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",
               4},
           Case{
               "_CLLocationCoordinate2DMake",
               "/System/Library/Frameworks/CoreLocation.framework/CoreLocation",
               2},
           Case{"___sincos_stret", "/usr/lib/libSystem.B.dylib", 2},
       }) {
    SCOPED_TRACE(C.Name);
    HFAImportVeneerFixture F(C.Name, C.Provider);
    ASSERT_TRUE(F.Result.Success) << F.Result.Error;
    ASSERT_EQ(F.Result.HighFuncs.size(), 1U);
    EXPECT_FALSE(F.Result.HighFuncs[0].SourceTypeHint);
    ASSERT_EQ(F.infer(), 1U);
    const auto &Hint = F.Options.SourceTypeHints.at(F.Entry);
    const auto Declared = darwinRuntimeSourceCallHint(F.Image, F.Slot);
    ASSERT_TRUE(Declared);
    EXPECT_TRUE(equalSourceABIs(Hint, Declared->Signature));
    EXPECT_EQ(Hint.ReturnComponents.size(), C.Components);
    // A declaration candidate must survive a fresh lift and all ordinary
    // publication checks with every record field still present.
    auto Relifted = Pipeline().run(F.Image, F.Context, F.Options);
    ASSERT_TRUE(Relifted.Success) << Relifted.Error;
    ASSERT_EQ(Relifted.HighFuncs.size(), 1U);
    const auto &Function = Relifted.HighFuncs[0];
    ASSERT_TRUE(Function.SourceTypeHint);
    EXPECT_TRUE(equalSourceABIs(*Function.SourceTypeHint, Hint));
    std::map<va_t, const HighFunc *> Functions{{F.Entry, &Function}};
    const auto Bound = sdk::bindObjCSourceReferences(Function, F.Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    const auto Allowed = [&](const HighExpr &Call) {
      return sdk::objcSourceCallBound(Call, F.Image, Functions);
    };
    ASSERT_EQ(Relifted.FunctionAudits.size(), 1U);
    EXPECT_TRUE(sdk::sourceBodyLimitation(Bound.Function, Hint,
                                          &Relifted.FunctionAudits[0], Allowed)
                    .empty());
  }
}

TEST(NativeSourceHints, HFAImportVeneerRequiresCurrentCodeAndImport) {
  HFAImportVeneerFixture F("___sincos_stret", "/usr/lib/libSystem.B.dylib");
  ASSERT_TRUE(darwinHFAImportVeneerSourceABI(F.Image, F.Entry));
  for (unsigned Mutation = 0; Mutation != 22; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = F.Image;
    va_t Entry = F.Entry;
    switch (Mutation) {
    case 0:
    case 1:
    case 2:
      Image.Segments[0].Data[4 * Mutation] ^= 1;
      break;
    case 3:
      Image.Segments[0].Flags =
          Image.Segments[0].Flags | SegmentFlags::Writable;
      break;
    case 4:
      Image.Segments[1].Flags =
          Image.Segments[1].Flags | SegmentFlags::Writable;
      Image.Sections[1].Flags =
          Image.Sections[1].Flags | SegmentFlags::Writable;
      break;
    case 5:
      Image.Sections.push_back(Image.Sections[0]);
      break;
    case 6:
      Image.Sections.push_back(Image.Sections[1]);
      break;
    case 7:
      Image.Segments[0].Data.resize(11);
      break;
    case 8:
      Image.DyldBindSlots[F.Slot].WeakImport = true;
      break;
    case 9:
      Image.DyldBindSlots[F.Slot].Addend = 8;
      break;
    case 10:
      Image.DyldBindSlots[F.Slot].Module = "/tmp/libSystem.B.dylib";
      break;
    case 11:
      Image.DynInfo.NeededLibs.clear();
      break;
    case 12:
      Image.ImportPtrSlots[F.Slot] = "_sin";
      break;
    case 13:
      Image.DyldBindSlots.clear();
      break;
    case 14:
      Image.ConflictingImportStorageSlots.insert(F.Slot);
      break;
    case 15:
      Image.CodePtrRelocSlots.insert(F.Entry + 8);
      break;
    case 16:
      Image.MachOChainedFixupsAmbiguous = true;
      break;
    case 17:
      Image.Arch = Arch::X64;
      break;
    case 18:
      Image.Format = BinaryFormat::ELF;
      break;
    case 19:
      Image.IsRelocatable = true;
      break;
    case 20:
      Image.Bits = Bitness::Bits32;
      break;
    case 21:
      ++Entry;
      break;
    }
    EXPECT_FALSE(darwinHFAImportVeneerSourceABI(Image, Entry));
  }
}

TEST(NativeSourceHints, HFAImportVeneerNeedsACompleteLiftBeforeRelifting) {
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    HFAImportVeneerFixture F("___sincos_stret", "/usr/lib/libSystem.B.dylib");
    ASSERT_TRUE(F.Result.Success) << F.Result.Error;
    ASSERT_EQ(F.Result.FunctionAudits.size(), 1U);
    auto &Audit = F.Result.FunctionAudits[0];
    switch (Mutation) {
    case 0:
      Audit.Disposition = PipelineFunctionDisposition::Candidate;
      break;
    case 1:
      Audit.HasLowIR = false;
      break;
    case 2:
      Audit.MedIRVerified = false;
      break;
    case 3:
      --Audit.LiftedInstructions;
      break;
    case 4:
      Audit.DecodeFailures.push_back(F.Entry);
      break;
    case 5:
      Audit.TruncatedPaths.push_back(F.Entry);
      break;
    case 6:
      ++Audit.Entry;
      break;
    }
    EXPECT_EQ(F.infer(), 0U);
    EXPECT_TRUE(F.Options.SourceTypeHints.empty());
  }
}

TEST(NativeSourceHints, HFAImportVeneerDoesNotGuessOtherDeclarations) {
  for (const auto Name : {"_sin", "_unknown_hfa"}) {
    HFAImportVeneerFixture F(Name, "/usr/lib/libSystem.B.dylib");
    EXPECT_FALSE(darwinHFAImportVeneerSourceABI(F.Image, F.Entry));
  }
  HFAImportVeneerFixture Indirect(
      "_CGAffineTransformMakeScale",
      "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics");
  const auto Declaration =
      darwinRuntimeSourceCallHint(Indirect.Image, Indirect.Slot);
  ASSERT_TRUE(Declaration);
  EXPECT_EQ(Declaration->Signature.ReturnLocation.Kind,
            SourceABICarrierKind::IndirectResultPointer);
  EXPECT_FALSE(darwinHFAImportVeneerSourceABI(Indirect.Image, Indirect.Entry));
}

TEST(NativeSourceHints, IndirectResultTailCallRetainsExplicitOutputAddress) {
  HFAImportVeneerFixture F(
      "_CGAffineTransformMakeScale",
      "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics");
  // The source function owns a direct tail, independently of the import
  // veneer's indirect transfer. Keep that original veneer as its target.
  auto &Code = F.Image.Segments[0];
  Code.Size = Code.FileSz = 0x200;
  Code.Data.resize(0x200);
  std::copy_n(Code.Data.begin(), 12, Code.Data.begin() + 0x100);
  llvm::support::endian::write32le(Code.Data.data(), 0x14000040);
  F.Image.Sections[0].Size = F.Image.Sections[0].FileSz = 0x200;
  F.Image.Segments[1].FileOff = F.Image.Sections[1].FileOff = 0x200;
  F.Image.Symbols[0].Size = 4;
  F.Image.Symbols.push_back({"import_veneer", F.Entry + 0x100, 12, true});
  F.Options.OnlyFunctionEntries.insert(F.Entry + 0x100);
  F.Result = Pipeline().run(F.Image, F.Context, F.Options);
  ASSERT_TRUE(F.Result.Success) << F.Result.Error;
  std::map<va_t, std::string> Diagnostics;
  ASSERT_EQ(sdk::inferObjCNativeDependencies(F.Image, F.Result, F.Options,
                                             Diagnostics, {F.Entry}),
            1U)
      << Diagnostics[F.Entry];
  const auto &Hint = F.Options.SourceTypeHints.at(F.Entry);
  EXPECT_EQ(Hint.Origin, SourceFunctionTypeHint::OriginKind::NativeAnalysis);
  EXPECT_EQ(Hint.ReturnType->Kind, NdTypeKind::Void);
  EXPECT_EQ(Hint.ReturnLocation.Kind, SourceABICarrierKind::None);
  ASSERT_EQ(Hint.Parameters.size(), 3U);
  EXPECT_EQ(Hint.Parameters[0].Type->Kind, NdTypeKind::Float);
  EXPECT_EQ(Hint.Parameters[1].Type->Kind, NdTypeKind::Float);
  EXPECT_EQ(Hint.Parameters[2].Location.RegisterOffset, a64reg::X8);
  EXPECT_EQ(Hint.Parameters[2].Location.ValueBytes, 8U);
  auto Fresh = Pipeline().run(F.Image, F.Context, F.Options);
  ASSERT_TRUE(Fresh.Success) << Fresh.Error;
  const auto Found = std::find_if(
      Fresh.HighFuncs.begin(), Fresh.HighFuncs.end(),
      [&](const auto &Function) { return Function.Entry == F.Entry; });
  ASSERT_NE(Found, Fresh.HighFuncs.end());
  const auto &Function = *Found;
  ASSERT_TRUE(Function.SourceTypeHint);
  EXPECT_TRUE(equalSourceABIs(*Function.SourceTypeHint, Hint));
  unsigned Stores = 0;
  walkStmts(Function.Body, [&](const HighStmt &Statement) {
    Stores += Statement.Kind == StmtKind::Store;
  });
  EXPECT_EQ(Stores, 6U);
  const auto Bound = sdk::bindObjCSourceReferences(Function, F.Image);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  std::map<va_t, const HighFunc *> Functions{{F.Entry, &Function}};
  const auto Allowed = [&](const HighExpr &Call) {
    return sdk::objcSourceCallBound(Call, F.Image, Functions);
  };
  const auto Audit =
      std::find_if(Fresh.FunctionAudits.begin(), Fresh.FunctionAudits.end(),
                   [&](const auto &A) { return A.Entry == F.Entry; });
  ASSERT_NE(Audit, Fresh.FunctionAudits.end());
  EXPECT_TRUE(sdk::sourceBodyLimitation(Bound.Function, Hint, &*Audit, Allowed)
                  .empty());
}

TEST(NativeSourceHints, CFunctionParameterCallKeepsTheCompleteVoidCallback) {
  using namespace c_function_parameter_test;
  Fixture F;
  ASSERT_TRUE(F.Result.Success) << F.Result.Error;
  ASSERT_TRUE(F.low());
  const auto Hints =
      buildCFunctionParameterCallHints(F.Image, *F.low(), F.Signature);
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(Call);
  ASSERT_TRUE(Hint.FunctionParameterCall);
  EXPECT_EQ(Hint.FunctionParameterCall->Parameter, 1U);
  EXPECT_EQ(Hint.FunctionParameterCall->FunctionEntry, Entry);
  EXPECT_EQ(Hint.FunctionParameterCall->Site.Instruction, Call);
  EXPECT_EQ(Hint.FunctionParameterCall->Site.Sequence, 1);
  EXPECT_EQ(Hint.Signature.Convention,
            SourceFunctionTypeHint::ConventionKind::C);
  EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Void);
  ASSERT_TRUE(F.high());
  unsigned Calls = 0;
  walkStmts(F.high()->Body, [&](const HighStmt &Statement) {
    forEachExpr(Statement, [&](const ExprPtr &Expression) {
      if (Expression && Expression->SourceCallHint &&
          Expression->SourceCallHint->FunctionParameterCall) {
        ++Calls;
        EXPECT_EQ(Statement.Kind, StmtKind::Call);
        EXPECT_EQ(Expression->Type->Kind, NdTypeKind::Void);
        EXPECT_TRUE(isCFunctionParameterSourceCall(*Expression, *F.high(),
                                                   Arch::AArch64));
      }
    });
  });
  EXPECT_EQ(Calls, 1U);
}

TEST(NativeSourceHints,
     CFunctionParameterCallRejectsIncompleteOrChangedEvidence) {
  using namespace c_function_parameter_test;
  Fixture F;
  ASSERT_TRUE(F.Result.Success) << F.Result.Error;
  ASSERT_TRUE(F.low());
  for (unsigned Case = 0; Case < 21; ++Case) {
    SCOPED_TRACE(Case);
    auto Image = F.Image;
    auto Low = *F.low();
    auto Signature = F.Signature;
    auto &Block = Low.Blocks.front();
    auto OpAt = [&](va_t Address, int Sequence) -> LowOp & {
      for (auto &Op : Block.Ops)
        if (Op.Addr == Address && Op.Seq == Sequence)
          return Op;
      throw std::runtime_error("missing fixture operation");
    };
    const auto Word = [&](unsigned Index, uint32_t Value) {
      llvm::support::endian::write32le(
          Image.Segments[0].Data.data() + Index * 4, Value);
    };
    switch (Case) {
    case 0:
      Word(3, 0xaa0003f3);
      break;
    case 1:
      Word(7, 0xd63f0280);
      break;
    case 2:
      OpAt(Call, 0).Inputs[0] = NdVar::cst(Call + 8, 8);
      break;
    case 3:
      OpAt(Call, 1).Inputs[0].Size = 4;
      break;
    case 4:
      Block.InstructionBoundaries.clear();
      break;
    case 5:
      --Low.DecodedInstructionCount;
      break;
    case 6:
      Low.TruncatedPathAddresses.push_back(Entry);
      break;
    case 7:
      Low.UnsupportedInstructionAddresses.push_back(Entry);
      break;
    case 8:
      Image.DyldBindSlots.at(runtime_function_address_test::Slot).WeakImport =
          true;
      break;
    case 9:
      Image.DyldBindSlots.at(runtime_function_address_test::Slot).Module =
          "/untrusted/libswiftCore.dylib";
      break;
    case 10:
      Image.DyldBindSlots.at(runtime_function_address_test::Slot).Addend = 8;
      break;
    case 11:
      Signature.Parameters[1].Type = NdType::makePtr(NdType::makeVoid());
      break;
    case 12:
      Signature.Parameters[1].Type = NdType::makePtr(NdType::makeFunc(
          NdType::makeStruct({NdType::makeInt(8), NdType::makeInt(8)}),
          {Signature.Parameters[0].Type}));
      break;
    case 13:
      OpAt(Entry + 12, 0).Output.Offset = a64reg::X20;
      break;
    case 14:
      OpAt(Entry + 16, 0).Opcode = NdOp::INTRINSIC;
      break;
    case 15:
      Block.Preds.push_back(Block.Id);
      Block.Succs.push_back(Block.Id);
      break;
    case 16:
      Image.Segments[0].Flags =
          Image.Segments[0].Flags | SegmentFlags::Writable;
      break;
    case 17:
      Signature.Parameters[1].Location.ExtendTo32Bits = true;
      break;
    case 18:
      Image.Format = BinaryFormat::ELF;
      break;
    case 19:
      OpAt(Entry + 20, 1).Inputs[0] = NdVar::cst(0x9999, 8);
      break;
    case 20:
      Image.DyldBindSlots.erase(runtime_function_address_test::Slot);
      break;
    }
    EXPECT_TRUE(
        buildCFunctionParameterCallHints(Image, Low, Signature).empty());
  }
}

TEST(NativeSourceHints, CFunctionParameterCallRequiresAgreementAtTheCFGJoin) {
  using namespace c_function_parameter_test;
  Fixture Same(true, true);
  ASSERT_TRUE(Same.Result.Success) << Same.Result.Error;
  ASSERT_TRUE(Same.low());
  const auto Valid =
      buildCFunctionParameterCallHints(Same.Image, *Same.low(), Same.Signature);
  ASSERT_EQ(Valid.size(), 1U);
  EXPECT_EQ(Valid.begin()->first, Same.CallAddress);
  Fixture Different(true, false);
  ASSERT_TRUE(Different.Result.Success) << Different.Result.Error;
  ASSERT_TRUE(Different.low());
  EXPECT_TRUE(buildCFunctionParameterCallHints(
                  Different.Image, *Different.low(), Different.Signature)
                  .empty());
  auto Cyclic = *Same.low();
  auto &Last = Cyclic.Blocks.back();
  Last.Succs.push_back(Last.Id);
  Last.Preds.push_back(Last.Id);
  EXPECT_TRUE(
      buildCFunctionParameterCallHints(Same.Image, Cyclic, Same.Signature)
          .empty());
}

TEST(NativeSourceHints, SpecializedURLArrayForceCastHasNoHiddenArguments) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  constexpr const char *Name =
      "_$ss15_arrayForceCastySayq_GSayxGr0_lF10Foundation3URLV_AFSgTg5";
  Image.Symbols.push_back({Name, 0x1000, 0, true});
  const auto Hint = sdk::swiftMangledURLArrayForceCastSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  ASSERT_EQ(Hint->Parameters.size(), 1U);
  EXPECT_EQ(Hint->Parameters[0].Type->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Hint->Parameters[0].TheRole,
            SourceParameterTypeHint::Role::Ordinary);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(Hint->Parameters[0].Location.ValueBytes, 8U);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Hint->ReturnLocation.RegisterOffset, a64reg::X0);
  EXPECT_EQ(Hint->ReturnLocation.ValueBytes, 8U);
  EXPECT_TRUE(Hint->ReturnComponents.empty());
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  for (const char *Other :
       {"_$ss15_arrayForceCastySayq_GSayxGr0_lF",
        "_$ss15_arrayForceCastySayq_GSayxGr0_lF10Foundation3URLV_AFTg5",
        "_$ss15_arrayForceCastySayq_GSayxGr0_lF10Foundation3URLVSg_AFTg5",
        "_$ss15_arrayForceCastySayq_GSayxGr0_lFSi_SiSgTg5",
        "_$ss15_arrayForceCastySayq_GSayxGr0_lF10Foundation3URLV_AFSgTg0",
        "_$ss15_arrayForceCastySayq_GSayxGr0_lF10Foundation3URLV_AFSgTg5To",
        "_$ss15_arrayForceCastySayq_GSayxGKr0_lF10Foundation3URLV_AFSgTg5",
        "_$ss15_arrayForceCastySayq_GSayxGr0_lF10Foundation3URLV_"
        "AFSgTg5junk"}) {
    SCOPED_TRACE(Other);
    auto Wrong = Image;
    Wrong.Symbols[0].Name = Other;
    EXPECT_FALSE(sdk::swiftMangledURLArrayForceCastSourceABI(Wrong, 0x1000));
  }
  auto Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(sdk::swiftMangledURLArrayForceCastSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].IsFunc = false;
  EXPECT_FALSE(sdk::swiftMangledURLArrayForceCastSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.IsRelocatable = true;
  EXPECT_FALSE(sdk::swiftMangledURLArrayForceCastSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Format = BinaryFormat::ELF;
  EXPECT_FALSE(sdk::swiftMangledURLArrayForceCastSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Arch = Arch::X64;
  EXPECT_FALSE(sdk::swiftMangledURLArrayForceCastSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Bits = Bitness::Bits32;
  EXPECT_FALSE(sdk::swiftMangledURLArrayForceCastSourceABI(Wrong, 0x1000));
  EXPECT_FALSE(sdk::swiftMangledURLArrayForceCastSourceABI(Image, 0x1100));
}

TEST(NativeSourceHints, SpecializedURLArrayBufferKeepsInoutSwiftSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  const std::string Stem = "_$ss15ContiguousArrayV16_"
                           "createNewBuffer14bufferIsUnique15minimumCapacity"
                           "13growForAppendySb_SiSbtF";
  const std::string Name = Stem + "10Foundation3URLVSg_Tg5";
  Image.Symbols.push_back({Name, 0x1000, 0, true});
  const auto Hint = sdk::swiftMangledURLArrayBufferSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Hint->Parameters.size(), 4U);
  for (size_t I = 0; I < 3; ++I) {
    EXPECT_EQ(Hint->Parameters[I].TheRole,
              SourceParameterTypeHint::Role::Ordinary);
    EXPECT_EQ(Hint->Parameters[I].Location.RegisterOffset, a64reg::X0 + I * 8);
    EXPECT_EQ(Hint->Parameters[I].Location.ValueBytes, I == 1 ? 8U : 1U);
  }
  EXPECT_EQ(Hint->Parameters[1].Type->Kind, NdTypeKind::Int);
  EXPECT_TRUE(Hint->Parameters[1].Type->IsSigned);
  EXPECT_EQ(Hint->Parameters[3].Type->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Hint->Parameters[3].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[3].Location.RegisterOffset, a64reg::X20);
  EXPECT_EQ(Hint->Parameters[3].Location.ValueBytes, 8U);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  std::vector<std::string> Invalid = {Stem,
                                      Stem + "10Foundation3URLV_Tg5",
                                      Stem + "SiSg_Tg5",
                                      Stem + "10Foundation3URLVSgSg_Tg5",
                                      Stem + "10Foundation3URLVSg_Tg0",
                                      Name + "Tm",
                                      Name + "To",
                                      Name + "TA",
                                      Name + "junk"};
  auto Other = Name;
  Other.replace(Other.find("_createNewBuffer"), 16, "_createNewBuffez");
  Invalid.push_back(Other);
  Other = Name;
  Other.replace(Other.find("ySb_SiSbtF"), 10, "ySb_SiSbtKF");
  Invalid.push_back(Other);
  Other = Name;
  Other.replace(Other.find("ySb_SiSbtF"), 10, "ySb_SiSftF");
  Invalid.push_back(Other);
  for (const auto &Bad : Invalid) {
    SCOPED_TRACE(Bad);
    auto Wrong = Image;
    Wrong.Symbols[0].Name = Bad;
    EXPECT_FALSE(sdk::swiftMangledURLArrayBufferSourceABI(Wrong, 0x1000));
  }
  for (unsigned Case = 0; Case < 6; ++Case) {
    auto Wrong = Image;
    switch (Case) {
    case 0:
      Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
      break;
    case 1:
      Wrong.Symbols[0].IsFunc = false;
      break;
    case 2:
      Wrong.IsRelocatable = true;
      break;
    case 3:
      Wrong.Format = BinaryFormat::ELF;
      break;
    case 4:
      Wrong.Arch = Arch::X64;
      break;
    case 5:
      Wrong.Bits = Bitness::Bits32;
      break;
    }
    EXPECT_FALSE(sdk::swiftMangledURLArrayBufferSourceABI(Wrong, 0x1000))
        << Case;
  }
  EXPECT_FALSE(sdk::swiftMangledURLArrayBufferSourceABI(Image, 0x1100));
}

namespace {
TEST(NativeSourceHints, SpecializedURLArrayAppendKeepsOwnedValueAndInoutSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  const std::string Name =
      "_$sSa6append10contentsOfyqd__n_t7ElementQyd__RszSTRd__lF"
      "10Foundation3URLVSg_SayAHGTg5";
  Image.Symbols.push_back({Name, 0x1000, 0, true});
  const auto Hint = sdk::swiftMangledURLArrayAppendSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Hint->Parameters.size(), 2U);
  for (const auto &Parameter : Hint->Parameters) {
    EXPECT_EQ(Parameter.Type->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Parameter.Location.ValueBytes, 8U);
  }
  EXPECT_EQ(Hint->Parameters[0].TheRole,
            SourceParameterTypeHint::Role::Ordinary);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(Hint->Parameters[1].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;
  std::vector<std::string> Invalid;
  for (const auto &[From, To] :
       {std::pair<const char *, const char *>{"6append", "6appenz"},
        {"10contentsOf", "10contentzOf"},
        {"yqd__n_t", "yqd___t"},
        {"STRd__", "SHRd__"},
        {"Rsz", "RszRlz"},
        {"10Foundation", "10Foundatiox"},
        {"3URLV", "3URLC"},
        {"URLVSg_", "URLV_"},
        {"URLVSg_", "URLVSgSg_"},
        {"_SayAHG", "_SaySiG"},
        {"Tg5", "Tg0"}}) {
    auto Changed = Name;
    Changed.replace(Changed.find(From), std::string(From).size(), To);
    Invalid.push_back(Changed);
  }
  for (const char *Suffix : {"Tm", "To", "TA", "junk"})
    Invalid.push_back(Name + Suffix);
  Invalid.push_back(Name.substr(0, Name.find("10Foundation")));
  for (const auto &Changed : Invalid) {
    auto Wrong = Image;
    Wrong.Symbols[0].Name = Changed;
    EXPECT_FALSE(sdk::swiftMangledURLArrayAppendSourceABI(Wrong, 0x1000))
        << Changed;
  }
  for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
    auto Wrong = Image;
    switch (Mutation) {
    case 0:
      Wrong.Symbols.push_back(Wrong.Symbols[0]);
      break;
    case 1:
      Wrong.Symbols[0].IsFunc = false;
      break;
    case 2:
      Wrong.IsRelocatable = true;
      break;
    case 3:
      Wrong.Format = BinaryFormat::ELF;
      break;
    case 4:
      Wrong.Arch = Arch::X64;
      break;
    case 5:
      Wrong.Bits = Bitness::Bits32;
      break;
    case 6:
      Wrong.Segments[0].Flags = SegmentFlags::Readable;
      break;
    }
    EXPECT_FALSE(sdk::swiftMangledURLArrayAppendSourceABI(Wrong, 0x1000));
  }
  EXPECT_FALSE(sdk::swiftMangledURLArrayAppendSourceABI(Image, 0x1100));
}

struct MergedURLBufferFixture {
  static constexpr va_t Wrapper = 0x1000, Helper = 0x1100, Slot = 0x2000;
  static constexpr const char *HelperName =
      "_$ss12_ArrayBufferV20_consumeAndCreateNew14bufferIsUnique"
      "15minimumCapacity13growForAppendAByxGSb_SiSbtF10Foundation3URLVSg_"
      "Tg5Tm";
  BinaryImage Image;
  llvm::LLVMContext Context;
  PipelineResult Result;
  void word(unsigned Index, uint32_t Value) {
    llvm::support::endian::write32le(Image.Segments[0].Data.data() + Index * 4,
                                     Value);
  }
  MergedURLBufferFixture() {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.Entry = Wrapper;
    Segment Text;
    Text.VA = Wrapper;
    Text.Size = Text.FileSz = 0x200;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(Text.Size);
    Image.Segments.push_back(std::move(Text));
    Section Code;
    Code.Name = "__text";
    Code.VA = Wrapper;
    Code.Size = Code.FileSz = 0x200;
    Code.Flags = Image.Segments[0].Flags;
    Code.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    Image.Sections.push_back(Code);
    Segment Data;
    Data.VA = Slot;
    Data.FileOff = 0x200;
    Data.Size = Data.FileSz = 8;
    Data.Flags = SegmentFlags::Readable;
    Data.Data.resize(8);
    Image.Segments.push_back(std::move(Data));
    Section Got;
    Got.Name = "__got";
    Got.VA = Slot;
    Got.FileOff = 0x200;
    Got.Size = Got.FileSz = 8;
    Got.Flags = SegmentFlags::Readable;
    Got.Type = llvm::MachO::S_NON_LAZY_SYMBOL_POINTERS;
    Image.Sections.push_back(Got);
    Image.ImportPtrSlots[Slot] = "_swift_release";
    Image.DyldBindSlots[Slot] = {"_swift_release", 0,
                                 "/usr/lib/swift/libswiftCore.dylib", false};
    Image.Symbols = {{"_$ss15ContiguousArrayV16_createNewBuffer14bufferIsUnique"
                      "15minimumCapacity13growForAppendySb_SiSbtF"
                      "10Foundation3URLVSg_Tg5",
                      Wrapper, 36, true},
                     {HelperName, Helper, 4, true}};
    const std::array<uint32_t, 9> Words{0xa9bf7bfd, 0x910003fd, 0xf9400283,
                                        0xb0000004, 0xf9400084, 0x9400003b,
                                        0xf9000280, 0xa8c17bfd, 0xd65f03c0};
    for (unsigned I = 0; I < Words.size(); ++I)
      word(I, Words[I]);
    word((Helper - Wrapper) / 4, 0xd65f03c0);
    const auto Hint = sdk::swiftMangledURLArrayBufferSourceABI(Image, Wrapper);
    EXPECT_TRUE(Hint);
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {Wrapper, Helper};
    if (Hint)
      Options.SourceTypeHints.emplace(Wrapper, *Hint);
    Result = Pipeline().run(Image, Context, Options);
    EXPECT_TRUE(Result.Success) << Result.Error;
    ObjCMethod Root;
    Root.Implementation = Wrapper;
    Root.Status = "supported";
    Root.TypeHint = Hint;
    Image.ObjCMethods.push_back(std::move(Root));
  }
  std::optional<SourceFunctionTypeHint> hint() {
    return sdk::swiftMergedURLArrayBufferSourceABI(Image, Helper, Result);
  }
};
} // namespace

TEST(NativeSourceHints, MergedURLBufferProvesTheExtraCReleaseCallback) {
  MergedURLBufferFixture F;
  const auto Hint = F.hint();
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  ASSERT_EQ(Hint->Parameters.size(), 5U);
  for (size_t I = 0; I < 5; ++I) {
    EXPECT_EQ(Hint->Parameters[I].TheRole,
              SourceParameterTypeHint::Role::Ordinary);
    EXPECT_EQ(Hint->Parameters[I].Location.RegisterOffset, a64reg::X0 + 8 * I);
    EXPECT_EQ(Hint->Parameters[I].Location.ValueBytes,
              I == 0 || I == 2 ? 1U : 8U);
  }
  EXPECT_TRUE(Hint->Parameters[1].Type->IsSigned);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Hint->ReturnLocation.RegisterOffset, a64reg::X0);
  const auto &Callback = Hint->Parameters[4].Type;
  ASSERT_EQ(Callback->Kind, NdTypeKind::Ptr);
  ASSERT_TRUE(Callback->Pointee);
  ASSERT_EQ(Callback->Pointee->Kind, NdTypeKind::Func);
  EXPECT_EQ(Callback->Pointee->RetType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Callback->Pointee->ParamTypes.size(), 1U);
  EXPECT_EQ(Callback->Pointee->ParamTypes[0]->Kind, NdTypeKind::Ptr);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  PipelineOptions Options;
  std::map<va_t, std::string> Diagnostics;
  EXPECT_EQ(
      sdk::inferObjCNativeDependencies(F.Image, F.Result, Options, Diagnostics),
      1U);
  ASSERT_TRUE(Options.SourceTypeHints.count(F.Helper));
  EXPECT_TRUE(equalSourceABIs(*Hint, Options.SourceTypeHints.at(F.Helper)));
  const auto *High =
      sdk::swift_merged_array_detail::unique(F.Result.HighFuncs, F.Helper);
  ASSERT_TRUE(High);
  EXPECT_FALSE(High->SourceTypeHint);
  auto *Audit = const_cast<PipelineFunctionAudit *>(
      sdk::swift_merged_array_detail::unique(F.Result.FunctionAudits,
                                             F.Helper));
  ASSERT_TRUE(Audit);
  Audit->MedIRVerified = false;
  PipelineOptions Incomplete;
  EXPECT_EQ(sdk::inferObjCNativeDependencies(F.Image, F.Result, Incomplete,
                                             Diagnostics),
            0U);
  EXPECT_TRUE(Incomplete.SourceTypeHints.empty());
}

TEST(NativeSourceHints, MergedURLBufferRejectsChangedMachineAndImportEvidence) {
  MergedURLBufferFixture F;
  ASSERT_TRUE(F.hint());
  for (unsigned I = 0; I < 9; ++I) {
    SCOPED_TRACE(I);
    const auto Original = llvm::support::endian::read32le(
        F.Image.Segments[0].Data.data() + 4 * I);
    F.word(I, Original ^ 1U);
    EXPECT_FALSE(F.hint());
    F.word(I, Original);
  }
  for (unsigned Case = 0; Case < 9; ++Case) {
    SCOPED_TRACE(Case);
    auto Original = F.Image;
    switch (Case) {
    case 0:
      F.Image.DyldBindSlots[F.Slot].WeakImport = true;
      break;
    case 1:
      F.Image.DyldBindSlots[F.Slot].Addend = 8;
      break;
    case 2:
      F.Image.DyldBindSlots[F.Slot].Module = "/other/libswiftCore.dylib";
      break;
    case 3:
      F.Image.ImportPtrSlots[F.Slot] = "_swift_retain";
      break;
    case 4:
      F.Image.DyldBindSlots.erase(F.Slot);
      break;
    case 5:
      F.Image.ConflictingImportStorageSlots.insert(F.Slot);
      break;
    case 6:
      F.Image.Segments[1].Flags =
          F.Image.Segments[1].Flags | SegmentFlags::Writable;
      break;
    case 7:
      F.Image.Sections.push_back(F.Image.Sections.back());
      break;
    case 8:
      F.Image.DataPtrRelocSlots.insert(F.Slot);
      break;
    }
    EXPECT_FALSE(F.hint());
    F.Image = std::move(Original);
  }
}

TEST(NativeSourceHints, MergedURLBufferRequiresCompleteTypedWrapperAndType) {
  MergedURLBufferFixture F;
  ASSERT_TRUE(F.hint());
  const std::string Name = MergedURLBufferFixture::HelperName;
  for (const auto &Bad :
       {Name.substr(0, Name.size() - 2), Name + "To", Name + "junk",
        std::string("_$ss12_ArrayBufferV20_consumeAndCreateNew14bufferIsUnique"
                    "15minimumCapacity13growForAppendAByxGSb_SiSbtFSiSg_Tg5Tm"),
        std::string("_$ss12_ArrayBufferV20_consumeAndCreateNew14bufferIsUnique"
                    "15minimumCapacity13growForAppendAByxGSb_SiSbtF"
                    "10Foundation3URLV_Tg5Tm")}) {
    F.Image.Symbols[1].Name = Bad;
    EXPECT_FALSE(F.hint()) << Bad;
  }
  F.Image.Symbols[1].Name = Name;
  for (unsigned Case = 0; Case < 13; ++Case) {
    SCOPED_TRACE(Case);
    const auto OriginalLow = F.Result.LowFuncs;
    const auto OriginalMed = F.Result.MedFuncs;
    const auto OriginalHigh = F.Result.HighFuncs;
    const auto OriginalAudits = F.Result.FunctionAudits;
    auto OriginalImage = F.Image;
    auto &Low = F.Result.LowFuncs.front();
    auto &Med = F.Result.MedFuncs.front();
    auto &High = F.Result.HighFuncs.front();
    auto &Audit = F.Result.FunctionAudits.front();
    switch (Case) {
    case 0:
      F.Result.SourceImage = nullptr;
      break;
    case 1:
      Audit.MedIRVerified = false;
      break;
    case 2:
      Audit.TruncatedPaths.push_back(F.Wrapper + 32);
      break;
    case 3:
      Med.SourceParametersBound = false;
      break;
    case 4:
      High.SourceTypeHint.reset();
      break;
    case 5:
      Low.Blocks[0].InstructionBoundaries.pop_back();
      break;
    case 6:
      Low.Blocks[0].Ops.back().Addr += 4;
      break;
    case 7:
      F.Result.FunctionAudits.push_back(Audit);
      break;
    case 8:
      F.Result.LowFuncs.push_back(Low);
      break;
    case 9:
      F.Image.Symbols.push_back({"_alias", F.Helper, 0, true});
      break;
    case 10:
      F.Image.Symbols.push_back({"_interior", F.Wrapper + 32, 0, true});
      break;
    case 11:
      F.Image.IsRelocatable = true;
      break;
    case 12:
      F.Image.Arch = Arch::X64;
      break;
    }
    EXPECT_FALSE(F.hint());
    F.Result.SourceImage = &F.Image;
    F.Result.LowFuncs = OriginalLow;
    F.Result.MedFuncs = OriginalMed;
    F.Result.HighFuncs = OriginalHigh;
    F.Result.FunctionAudits = OriginalAudits;
    F.Image = std::move(OriginalImage);
  }
}

TEST(NativeSourceHints, ExactMangledStringBundleFunctionKeepsPairResult) {
  constexpr llvm::StringLiteral Name =
      "_$s4main9localized_12languageCode6bundle5value7commentS2S_SSSgSo8"
      "NSBundleCSgS2StF";
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    BinaryImage Image;
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Architecture;
    Image.Bits = Bitness::Bits64;
    Segment Text;
    Text.VA = 0x1000;
    Text.Size = Text.FileSz = 0x100;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(0x100);
    Image.Segments.push_back(std::move(Text));
    Image.Symbols.push_back({Name.str(), 0x1000, 0, true});
    const auto Hint =
        sdk::swiftMangledStringBundleSourceABI(Image, 0x1000, true);
    ASSERT_TRUE(Hint);
    EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
    EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
    EXPECT_EQ(Hint->ReturnType->Size, 16U);
    ASSERT_EQ(Hint->ReturnComponents.size(), 2U);
    ASSERT_EQ(Hint->Parameters.size(), 9U);
    EXPECT_EQ(Hint->Parameters[8].Location.Kind, SourceABICarrierKind::Stack);
    if (Architecture == Arch::AArch64) {
      EXPECT_TRUE(
          swift_boolean_projection_detail::nativeEntry(Image, 0x1000, *Hint));
      auto Forged = *Hint;
      Forged.Parameters[0].Type = NdType::makePtr(NdType::makeVoid());
      EXPECT_FALSE(
          swift_boolean_projection_detail::nativeEntry(Image, 0x1000, Forged));
    }
    std::string Error;
    EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;
    EXPECT_FALSE(sdk::swiftMangledStringBundleSourceABI(Image, 0x1000, false));
    auto Wrong = Image;
    Wrong.Symbols[0].Name =
        "_$s4main9localized_12languageCode6bundle5value7commentS2S_SSSgSo8"
        "NSObjectCSgS2StF";
    EXPECT_FALSE(sdk::swiftMangledStringBundleSourceABI(Wrong, 0x1000, true));
    if (Architecture == Arch::AArch64)
      EXPECT_FALSE(
          swift_boolean_projection_detail::nativeEntry(Wrong, 0x1000, *Hint));
    Wrong = Image;
    Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
    EXPECT_FALSE(sdk::swiftMangledStringBundleSourceABI(Wrong, 0x1000, true));
  }
}

TEST(NativeSourceHints, ArrayStringValueInitializerRequiresExactPassThrough) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$s6Lottie16AnimationKeypathV4keysACSaySSG_tcfC", 0x1000, 0, true});
  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Blocks.emplace_back();
  Low.Blocks[0].StartAddr = 0x1000;
  LowOp Counter;
  Counter.Opcode = NdOp::INT_ADD;
  Counter.Output = NdVar::reg(a64reg::X8, 8);
  Counter.addInput(NdVar::reg(a64reg::X8, 8));
  Counter.addInput(NdVar::cst(1, 8));
  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(NdVar::reg(a64reg::X30, 8));
  Low.Blocks[0].Ops = {Counter, Return};
  const auto Hint =
      sdk::swiftMangledArrayStringValueInitializerSourceABI(Image, 0x1000, Low);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
  ASSERT_EQ(Hint->Parameters.size(), 1U);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(Hint->ReturnLocation.RegisterOffset, a64reg::X0);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Clobber = Low;
  Clobber.Blocks[0].Ops[0].Output = NdVar::reg(a64reg::X0, 4);
  EXPECT_FALSE(sdk::swiftMangledArrayStringValueInitializerSourceABI(
      Image, 0x1000, Clobber));
  auto Call = Low;
  Call.Blocks[0].Ops[0].Opcode = NdOp::CALL;
  EXPECT_FALSE(sdk::swiftMangledArrayStringValueInitializerSourceABI(
      Image, 0x1000, Call));
  Call.Blocks[0].Ops[0].Opcode = NdOp::BRANCH;
  EXPECT_FALSE(sdk::swiftMangledArrayStringValueInitializerSourceABI(
      Image, 0x1000, Call));
  auto BadReturn = Low;
  BadReturn.Blocks[0].Ops.back().Inputs[0] = NdVar::reg(a64reg::X0, 8);
  EXPECT_FALSE(sdk::swiftMangledArrayStringValueInitializerSourceABI(
      Image, 0x1000, BadReturn));
  auto Split = Low;
  Split.Blocks.emplace_back();
  EXPECT_FALSE(sdk::swiftMangledArrayStringValueInitializerSourceABI(
      Image, 0x1000, Split));
  auto Wrong = Image;
  Wrong.Symbols[0].Name = "_$s6Lottie16AnimationKeypathV4keysACSaySiG_tcfC";
  EXPECT_FALSE(sdk::swiftMangledArrayStringValueInitializerSourceABI(
      Wrong, 0x1000, Low));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(sdk::swiftMangledArrayStringValueInitializerSourceABI(
      Wrong, 0x1000, Low));
}

TEST(NativeSourceHints, ObjCExtensionBoolGetterUsesSwiftSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$sSo8NSBundleC3WMFE14isAppExtensionSbvg", 0x1000, 0, true});
  const auto Hint = sdk::swiftMangledObjCBoolMemberSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(Hint->ReturnType->Size, 1U);
  ASSERT_EQ(Hint->Parameters.size(), 1U);
  EXPECT_EQ(Hint->Parameters[0].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Method = Image;
  Method.Symbols[0].Name =
      "_$sSo14NSUserDefaultsC3WMFE34wmf_isTableOfContentsVisibleInlineSbyF";
  const auto MethodHint =
      sdk::swiftMangledObjCBoolMemberSourceABI(Method, 0x1000);
  ASSERT_TRUE(MethodHint);
  ASSERT_EQ(MethodHint->Parameters.size(), 1U);
  EXPECT_EQ(MethodHint->Parameters[0].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(MethodHint->Parameters[0].Location.RegisterOffset, a64reg::X20);
  EXPECT_EQ(MethodHint->ReturnType->Size, 1U);
  EXPECT_TRUE(validateSourceABI(*MethodHint, Error)) << Error;
  Method.Symbols[0].Name =
      "_$sSo14NSUserDefaultsC3WMFE34wmf_isTableOfContentsVisibleInlineSiyF";
  EXPECT_FALSE(sdk::swiftMangledObjCBoolMemberSourceABI(Method, 0x1000));

  auto Wrong = Image;
  Wrong.Symbols[0].Name = "_$sSo8NSBundleC3WMFE14isAppExtensionSbvp";
  EXPECT_FALSE(sdk::swiftMangledObjCBoolMemberSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name = "_$sSo8NSBundleC3WMFE14isAppExtensionSSvg";
  EXPECT_FALSE(sdk::swiftMangledObjCBoolMemberSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(sdk::swiftMangledObjCBoolMemberSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Arch = Arch::X64;
  EXPECT_FALSE(sdk::swiftMangledObjCBoolMemberSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints, SwiftObjCObjectVoidMethodUsesSwiftSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$s3WMF11ActionsViewC17willPerformActionyySo8UIButtonCF", 0x1000, 0,
       true});
  const auto Hint =
      sdk::swiftMangledObjCObjectVoidMethodSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Hint->Parameters.size(), 2U);
  EXPECT_EQ(Hint->Parameters[0].Type->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(Hint->Parameters[1].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Named = Image;
  Named.Symbols[0].Name =
      "_$s3WMF28WMFSavedPageSpotlightManagerC10addToIndex3urlySo5NSURLC_tF";
  const auto NamedHint =
      sdk::swiftMangledObjCObjectVoidMethodSourceABI(Named, 0x1000);
  ASSERT_TRUE(NamedHint);
  ASSERT_EQ(NamedHint->Parameters.size(), 2U);
  EXPECT_EQ(NamedHint->Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(NamedHint->Parameters[1].Location.RegisterOffset, a64reg::X20);
  EXPECT_TRUE(validateSourceABI(*NamedHint, Error)) << Error;
  Named.Symbols[0].Name += "Tf4nd_n";
  EXPECT_FALSE(sdk::swiftMangledObjCObjectVoidMethodSourceABI(Named, 0x1000));

  auto Wrong = Image;
  Wrong.Symbols[0].Name += "ToTm";
  EXPECT_FALSE(sdk::swiftMangledObjCObjectVoidMethodSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name = "_$s3WMF11ActionsViewC17willPerformActionyySiF";
  EXPECT_FALSE(sdk::swiftMangledObjCObjectVoidMethodSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(sdk::swiftMangledObjCObjectVoidMethodSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints, ObjCObjectPairVoidMethodUsesSwiftSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back({"_$s3WMF18CollectionViewCellC19setBackgroundColors_"
                           "8selectedySo7UIColorC_AGtF",
                           0x1000, 0, true});
  const auto Hint =
      sdk::swiftMangledObjCObjectPairVoidMethodSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Hint->Parameters.size(), 3U);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, a64reg::X1);
  EXPECT_EQ(Hint->Parameters[2].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[2].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name =
      "_$s3WMF18CollectionViewCellC19setBackgroundColors_8selectedySS_SStF";
  EXPECT_FALSE(
      sdk::swiftMangledObjCObjectPairVoidMethodSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name = "_$s3WMF18CollectionViewCellC19setBackgroundColors_"
                          "8selectedSbSo7UIColorC_AGtF";
  EXPECT_FALSE(
      sdk::swiftMangledObjCObjectPairVoidMethodSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name += "Z";
  EXPECT_FALSE(
      sdk::swiftMangledObjCObjectPairVoidMethodSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(
      sdk::swiftMangledObjCObjectPairVoidMethodSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Arch = Arch::X64;
  EXPECT_FALSE(
      sdk::swiftMangledObjCObjectPairVoidMethodSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints, UIColorIntAlphaAllocatorUsesMixedSwiftRegisters) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$sSo7UIColorC13WMFComponentsE_5alphaABSi_12CoreGraphics7CGFloatVtcfC",
       0x1000, 0, true});
  const auto Hint =
      sdk::swiftMangledUIColorIntAlphaAllocatorSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Ptr);
  ASSERT_EQ(Hint->Parameters.size(), 3U);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, a64reg::V0);
  EXPECT_EQ(Hint->Parameters[2].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[2].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name = "_$sSo7UIColorC13WMFComponentsE_5alphaABSi_SitcfC";
  EXPECT_FALSE(
      sdk::swiftMangledUIColorIntAlphaAllocatorSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name =
      "_$sSo7NSColorC13WMFComponentsE_5alphaABSi_12CoreGraphics7CGFloatVtcfC";
  EXPECT_FALSE(
      sdk::swiftMangledUIColorIntAlphaAllocatorSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name =
      "_$sSo7UIColorC13WMFComponentsE_5alphaABSi_12CoreGraphics7CGFloatVtcfc";
  EXPECT_FALSE(
      sdk::swiftMangledUIColorIntAlphaAllocatorSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(
      sdk::swiftMangledUIColorIntAlphaAllocatorSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints, UIColorIntAllocatorAuthenticatesNumericSwiftInput) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$sSo7UIColorC13WMFComponentsEyABSicfC", 0x1000, 0, true});
  const auto Hint =
      sdk::swiftMangledUIColorIntAllocatorSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Ptr);
  ASSERT_EQ(Hint->Parameters.size(), 2U);
  EXPECT_EQ(Hint->Parameters[0].Type->Kind, NdTypeKind::Int);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(Hint->Parameters[1].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name =
      "_$sSo7UIColorC13WMFComponentsE_5alphaABSi_12CoreGraphics7CGFloatVtcfC";
  EXPECT_FALSE(sdk::swiftMangledUIColorIntAllocatorSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name = "_$sSo7NSColorC13WMFComponentsEyABSicfC";
  EXPECT_FALSE(sdk::swiftMangledUIColorIntAllocatorSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name = "_$sSo7UIColorC13WMFComponentsEyABSicfc";
  EXPECT_FALSE(sdk::swiftMangledUIColorIntAllocatorSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(sdk::swiftMangledUIColorIntAllocatorSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Arch = Arch::X64;
  EXPECT_FALSE(sdk::swiftMangledUIColorIntAllocatorSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints, ZeroArgClassVoidAndBoolMethodsUseSwiftSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$s3WMF15LocationManagerC014stopMonitoringB0yyF", 0x1000, 0, true});
  const auto Hint = sdk::swiftMangledZeroArgClassMethodSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Hint->Parameters.size(), 1U);
  EXPECT_EQ(Hint->Parameters[0].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Private = Image;
  Private.Symbols[0].Name = "_$s3WMF18AlignedImageButtonC12adjustInsets33_"
                            "2AE2377B0A7FC7A2DEBED24238BE2C37LLyyF";
  const auto PrivateHint =
      sdk::swiftMangledZeroArgClassMethodSourceABI(Private, 0x1000);
  ASSERT_TRUE(PrivateHint);
  ASSERT_EQ(PrivateHint->Parameters.size(), 1U);
  EXPECT_EQ(PrivateHint->Parameters[0].Location.RegisterOffset, a64reg::X20);
  EXPECT_TRUE(validateSourceABI(*PrivateHint, Error)) << Error;

  auto Boolean = Image;
  Boolean.Symbols[0].Name =
      "_$s7WMFData21WMFHomeDataControllerC28communityFeaturedArticleIsOnSbyF";
  const auto BooleanHint =
      sdk::swiftMangledZeroArgClassMethodSourceABI(Boolean, 0x1000);
  ASSERT_TRUE(BooleanHint);
  EXPECT_EQ(BooleanHint->ReturnType->Kind, NdTypeKind::Int);
  EXPECT_EQ(BooleanHint->ReturnType->Size, 1U);
  ASSERT_EQ(BooleanHint->Parameters.size(), 1U);
  EXPECT_EQ(BooleanHint->Parameters[0].Location.RegisterOffset, a64reg::X20);
  EXPECT_TRUE(validateSourceABI(*BooleanHint, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name = "_$s3WMF15LocationManagerC014stopMonitoringB0SiyF";
  EXPECT_FALSE(sdk::swiftMangledZeroArgClassMethodSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name += "Z";
  EXPECT_FALSE(sdk::swiftMangledZeroArgClassMethodSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name = "_$sSo15LocationManagerC3WMFE014stopMonitoringB0yyF";
  EXPECT_FALSE(sdk::swiftMangledZeroArgClassMethodSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(sdk::swiftMangledZeroArgClassMethodSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints, SwiftClassScalarSetterUsesValueAndSwiftSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$s3WMF25ArticleCollectionViewCellC10isSelectedSbvs", 0x1000, 0, true});
  const auto Hint = sdk::swiftMangledClassScalarSetterSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Hint->Parameters.size(), 2U);
  EXPECT_EQ(Hint->Parameters[0].Type->Size, 1U);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(Hint->Parameters[1].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Integer = Image;
  Integer.Symbols[0].Name = "_$s6Lottie8LRUCacheC10countLimitSivs";
  const auto IntegerHint =
      sdk::swiftMangledClassScalarSetterSourceABI(Integer, 0x1000);
  ASSERT_TRUE(IntegerHint);
  EXPECT_EQ(IntegerHint->ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(IntegerHint->Parameters.size(), 2U);
  EXPECT_EQ(IntegerHint->Parameters[0].Type->Size, 8U);
  EXPECT_EQ(IntegerHint->Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(IntegerHint->Parameters[1].Location.RegisterOffset, a64reg::X20);
  EXPECT_TRUE(validateSourceABI(*IntegerHint, Error)) << Error;

  auto CGFloat = Image;
  CGFloat.Symbols[0].Name = "_$"
                            "s6Lottie23CompatibleAnimationViewC04loopC5Count12C"
                            "oreGraphics7CGFloatVvs";
  const auto CGFloatHint =
      sdk::swiftMangledClassScalarSetterSourceABI(CGFloat, 0x1000);
  ASSERT_TRUE(CGFloatHint);
  EXPECT_EQ(CGFloatHint->ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(CGFloatHint->Parameters.size(), 2U);
  EXPECT_EQ(CGFloatHint->Parameters[0].Type->Kind, NdTypeKind::Float);
  EXPECT_EQ(CGFloatHint->Parameters[0].Type->Size, 8U);
  EXPECT_EQ(CGFloatHint->Parameters[0].Location.Kind,
            SourceABICarrierKind::FloatingRegister);
  EXPECT_EQ(CGFloatHint->Parameters[0].Location.RegisterOffset,
            getTargetRegInfo(Arch::AArch64).FPParamRegs[0]);
  EXPECT_EQ(CGFloatHint->Parameters[1].Location.RegisterOffset, a64reg::X20);
  EXPECT_TRUE(validateSourceABI(*CGFloatHint, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name = "_$s3WMF25ArticleCollectionViewCellC10isSelectedSbvg";
  EXPECT_FALSE(sdk::swiftMangledClassScalarSetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name =
      "_$s3WMF25ArticleCollectionViewCellC17horizontalSpacingSdvs";
  EXPECT_FALSE(sdk::swiftMangledClassScalarSetterSourceABI(Wrong, 0x1000));
  Wrong = CGFloat;
  Wrong.Symbols[0].Name += "To";
  EXPECT_FALSE(sdk::swiftMangledClassScalarSetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(sdk::swiftMangledClassScalarSetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Arch = Arch::X64;
  EXPECT_FALSE(sdk::swiftMangledClassScalarSetterSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints, ProfiledObjCMergedCGFloatSetterHasVoidCABI) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  const std::array<uint8_t, 4> ProfileInput = {0xf3, 0x03, 0x03, 0xaa};
  const std::array<uint8_t, 12> ProfileUpdate = {
      0x68, 0x02, 0x40, 0xf9, 0x08, 0x05, 0x00, 0x91, 0x68, 0x02, 0x00, 0xf9};
  std::copy(ProfileInput.begin(), ProfileInput.end(), Text.Data.begin() + 0x18);
  std::copy(ProfileUpdate.begin(), ProfileUpdate.end(),
            Text.Data.begin() + 0x40);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$s3WMF18AlignedImageButtonC17horizontalSpacing12CoreGraphics"
       "7CGFloatVvsToTm",
       0x1000, 0, true});
  const auto Hint =
      sdk::swiftMangledProfiledObjCCGFloatSetterSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::C);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Hint->Parameters.size(), 5U);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, a64reg::X1);
  EXPECT_EQ(Hint->Parameters[2].Type->Kind, NdTypeKind::Float);
  EXPECT_EQ(Hint->Parameters[2].Location.Kind,
            SourceABICarrierKind::FloatingRegister);
  EXPECT_EQ(Hint->Parameters[2].Location.RegisterOffset,
            getTargetRegInfo(Arch::AArch64).FPParamRegs[0]);
  EXPECT_EQ(Hint->Parameters[3].Location.RegisterOffset, a64reg::X2);
  EXPECT_EQ(Hint->Parameters[4].Location.RegisterOffset, a64reg::X3);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name.pop_back();
  EXPECT_FALSE(
      sdk::swiftMangledProfiledObjCCGFloatSetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(
      sdk::swiftMangledProfiledObjCCGFloatSetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Arch = Arch::X64;
  EXPECT_FALSE(
      sdk::swiftMangledProfiledObjCCGFloatSetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Segments[0].Data[0x40] = 0;
  EXPECT_FALSE(
      sdk::swiftMangledProfiledObjCCGFloatSetterSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints, SwiftClassScalarGetterUsesSwiftSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$s3WMF24WMFAuthenticationManagerC20authStateIsTemporarySbvg", 0x1000,
       0, true});
  auto Hint = sdk::swiftMangledClassScalarGetterSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Int);
  EXPECT_EQ(Hint->ReturnType->Size, 1U);
  ASSERT_EQ(Hint->Parameters.size(), 1U);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto CGFloat = Image;
  CGFloat.Symbols[0].Name = "_$"
                            "s3WMF25ArticleCollectionViewCellC24swipeTranslatio"
                            "nWhenOpen12CoreGraphics7CGFloatVvg";
  Hint = sdk::swiftMangledClassScalarGetterSourceABI(CGFloat, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Float);
  EXPECT_EQ(Hint->ReturnType->Size, 8U);
  EXPECT_EQ(Hint->ReturnLocation.Kind, SourceABICarrierKind::FloatingRegister);
  EXPECT_EQ(Hint->ReturnLocation.RegisterOffset,
            getTargetRegInfo(Arch::AArch64).FPReturnReg);
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Wrong = CGFloat;
  Wrong.Symbols[0].Name += "To";
  EXPECT_FALSE(sdk::swiftMangledClassScalarGetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name.back() = 's';
  EXPECT_FALSE(sdk::swiftMangledClassScalarGetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(sdk::swiftMangledClassScalarGetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Arch = Arch::X64;
  EXPECT_FALSE(sdk::swiftMangledClassScalarGetterSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints, SwiftDoubleFloatingPropertyUsesFPLanes) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$sSd6LottieE7cgFloat12CoreGraphics7CGFloatVvg", 0x1000, 0, true});
  auto Hint = sdk::swiftMangledDoubleFloatingPropertySourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Float);
  EXPECT_EQ(Hint->ReturnType->Size, 8U);
  EXPECT_EQ(Hint->ReturnLocation.Kind, SourceABICarrierKind::FloatingRegister);
  EXPECT_EQ(Hint->ReturnLocation.RegisterOffset,
            getTargetRegInfo(Arch::AArch64).FPReturnReg);
  ASSERT_EQ(Hint->Parameters.size(), 1U);
  EXPECT_EQ(Hint->Parameters[0].Type->Kind, NdTypeKind::Float);
  EXPECT_EQ(Hint->Parameters[0].Location.Kind,
            SourceABICarrierKind::FloatingRegister);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset,
            getTargetRegInfo(Arch::AArch64).FPParamRegs[0]);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Initializer = Image;
  Initializer.Symbols[0].Name =
      "_$s6Lottie18CoreAnimationLayerC26CAMediaTimingConfigurationV10timeOffs"
      "etSdvpfi";
  Hint = sdk::swiftMangledDoubleFloatingPropertySourceABI(Initializer, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_TRUE(Hint->Parameters.empty());
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Float);
  EXPECT_EQ(Hint->ReturnLocation.Kind, SourceABICarrierKind::FloatingRegister);
  EXPECT_EQ(Hint->ReturnLocation.RegisterOffset,
            getTargetRegInfo(Arch::AArch64).FPReturnReg);
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name = "_$sSd6LottieE7cgFloatSdvg";
  EXPECT_FALSE(sdk::swiftMangledDoubleFloatingPropertySourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name += "To";
  EXPECT_FALSE(sdk::swiftMangledDoubleFloatingPropertySourceABI(Wrong, 0x1000));
  Wrong = Initializer;
  Wrong.Symbols[0].Name.replace(Wrong.Symbols[0].Name.find("Sdvpfi"), 6,
                                "Sivpfi");
  EXPECT_FALSE(sdk::swiftMangledDoubleFloatingPropertySourceABI(Wrong, 0x1000));
  // A generic Outer<T>.Inner.x has the same demangled owner shape, but Swift
  // passes a hidden metadata pointer to its property initializer.
  Wrong = Initializer;
  Wrong.Symbols[0].Name = "_$s10SomeModule5OuterC5InnerV1xSdvpfi";
  EXPECT_FALSE(sdk::swiftMangledDoubleFloatingPropertySourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(sdk::swiftMangledDoubleFloatingPropertySourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Arch = Arch::X64;
  EXPECT_FALSE(sdk::swiftMangledDoubleFloatingPropertySourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints, SwiftClassReferenceGetterUsesSwiftSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$s3WMF13ConfigurationC6routerAA6RouterCvg", 0x1000, 0, true});
  auto Hint = sdk::swiftMangledClassReferenceGetterSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Hint->ReturnLocation.RegisterOffset, a64reg::X0);
  ASSERT_EQ(Hint->Parameters.size(), 1U);
  EXPECT_EQ(Hint->Parameters[0].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  Image.Symbols[0].Name =
      "_$s13WMFComponents17WMFAppEnvironmentC15traitCollectionSo07UITraitE0Cvg";
  Hint = sdk::swiftMangledClassReferenceGetterSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Ptr);
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  Image.Symbols[0].Name = "_$s3WMF12TimelineViewC18squiggleShapeLayer33_"
                          "CCCFBA6168377C4D3BACC0F9F7AE2E61LLSo07CAShapeF0Cvg";
  Hint = sdk::swiftMangledClassReferenceGetterSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Ptr);
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name += "To";
  EXPECT_FALSE(sdk::swiftMangledClassReferenceGetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(sdk::swiftMangledClassReferenceGetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Arch = Arch::X64;
  EXPECT_FALSE(sdk::swiftMangledClassReferenceGetterSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints,
     SwiftClassOptionalExistentialGetterUsesIndirectResultAndSwiftSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$s7WMFData0A11EnvironmentC12basicServiceAA10WMFService_pSgvg", 0x1000,
       0, true});
  auto Hint =
      sdk::swiftMangledClassOptionalExistentialGetterSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Hint->Parameters.size(), 2U);
  EXPECT_EQ(Hint->Parameters[0].TheRole,
            SourceParameterTypeHint::Role::SwiftIndirectResult);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X8);
  EXPECT_EQ(Hint->Parameters[1].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name += "To";
  EXPECT_FALSE(
      sdk::swiftMangledClassOptionalExistentialGetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name.back() = 's';
  EXPECT_FALSE(
      sdk::swiftMangledClassOptionalExistentialGetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(
      sdk::swiftMangledClassOptionalExistentialGetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Arch = Arch::X64;
  EXPECT_FALSE(
      sdk::swiftMangledClassOptionalExistentialGetterSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints, ZeroArgClassInitializerUsesSwiftSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$s3WMF31WMFArticlePreviewViewControllerCACycfc", 0x1000, 0, true});
  const auto Hint =
      sdk::swiftMangledZeroArgClassInitializerSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Ptr);
  ASSERT_EQ(Hint->Parameters.size(), 1U);
  EXPECT_EQ(Hint->Parameters[0].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name = "_$s3WMF31WMFArticlePreviewViewControllerCACycfC";
  EXPECT_FALSE(
      sdk::swiftMangledZeroArgClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name =
      "_$s3WMF31WMFArticlePreviewViewControllerC5coderACSo7NSCoderC_tcfc";
  EXPECT_FALSE(
      sdk::swiftMangledZeroArgClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(
      sdk::swiftMangledZeroArgClassInitializerSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints,
     CGContextCGRectMethodKeepsOrdinaryAndSwiftContextInputs) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  const std::string Public =
      "_$s13RectMethodABI12DrawingOwnerC4draw2in4rectySo12CGContextRefa_"
      "So6CGRectVtF";
  const std::string Private = "_$s3WMF12TimelineViewC16drawVerticalLine33_"
                              "CCCFBA6168377C4D3BACC0F9F7AE2E61LL2in4rectySo12C"
                              "GContextRefa_So6CGRectVtF";
  for (const auto &Name : {Public, Private}) {
    Image.Symbols = {{Name, 0x1000, 0, true}};
    const auto Declaration =
        swiftMangledCGContextCGRectClassMethodDeclaration(Image, 0x1000);
    ASSERT_TRUE(Declaration) << Name;
    const auto &Hint = Declaration->Signature;
    EXPECT_EQ(Declaration->Module, Name == Public ? "RectMethodABI" : "WMF");
    EXPECT_EQ(Declaration->ClassName,
              Name == Public ? "DrawingOwner" : "TimelineView");
    EXPECT_EQ(Hint.Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
    EXPECT_EQ(Hint.ReturnType->Kind, NdTypeKind::Void);
    ASSERT_EQ(Hint.Parameters.size(), 3U);
    EXPECT_EQ(Hint.Parameters[0].Location.RegisterOffset, a64reg::X0);
    ASSERT_EQ(Hint.Parameters[1].Components.size(), 4U);
    EXPECT_EQ(Hint.Parameters[1].Type->Size, 32U);
    for (size_t I = 0; I != 4; ++I) {
      EXPECT_EQ(Hint.Parameters[1].Components[I].RegisterOffset, a64reg::V(I));
      EXPECT_EQ(Hint.Parameters[1].Components[I].ValueBytes, 8U);
    }
    EXPECT_EQ(Hint.Parameters[2].TheRole,
              SourceParameterTypeHint::Role::SwiftContext);
    EXPECT_EQ(Hint.Parameters[2].Location.RegisterOffset, a64reg::X20);
    std::string Error;
    EXPECT_TRUE(validateSourceABI(Hint, Error)) << Error;
    const auto Shared = swiftMangledReceiverClassMethodSourceABI(Image, 0x1000);
    ASSERT_TRUE(Shared);
    EXPECT_TRUE(equalSourceABIs(*Shared, Hint));
  }
  Image.Symbols = {{Public, 0x1000, 0, true}};
  for (const char *Name :
       {"_$s13RectMethodABI12DrawingOwnerC4draw2in4rectySo12CGContextRefa_"
        "So6CGRectVtFTo",
        "_$s13RectMethodABI12DrawingOwnerC4draw2in4rectySo12CGContextRefa_"
        "So6CGRectVtFTf4d_n",
        "_$s13RectMethodABI12DrawingOwnerC4draw2in4rectySo12CGContextRefa_"
        "So6CGRectVtFZ",
        "_$s13RectMethodABI12DrawingOwnerC4draw2in4rectSbSo12CGContextRefa_"
        "So6CGRectVtF",
        "_$s13RectMethodABI12DrawingOwnerC4draw2in4rectySo6CGRectV_"
        "So12CGContextRefatF",
        "_$s13RectMethodABI12DrawingOwnerC4draw2in4rectySo12CGContextRefa_"
        "So7CGPointVtF",
        "_$s13RectMethodABI12DrawingOwnerC4draw2in4rectySo12CGContextRefaSg_"
        "So6CGRectVtF",
        "_$s13RectMethodABI12DrawingOwnerC4draw2in4rectySo12CGContextRefC_"
        "So6CGRectVtF",
        "_$s13RectMethodABI12DrawingOwnerC4draw2in4rectySo10CGImageRefa_"
        "So6CGRectVtF",
        "_$s13RectMethodABI12DrawingOwnerC4draw2in4rectySo12CGContextRefa_"
        "So6CGRectVztF",
        "_$s13RectMethodABI12DrawingOwnerV4draw2in4rectySo12CGContextRefa_"
        "So6CGRectVtF"}) {
    auto Wrong = Image;
    Wrong.Symbols[0].Name = Name;
    EXPECT_FALSE(swiftMangledCGContextCGRectClassMethodSourceABI(Wrong, 0x1000))
        << Name;
  }
  for (unsigned Case = 0; Case != 5; ++Case) {
    auto Wrong = Image;
    if (Case == 0)
      Wrong.Arch = Arch::X64;
    if (Case == 1)
      Wrong.Format = BinaryFormat::ELF;
    if (Case == 2)
      Wrong.IsRelocatable = true;
    if (Case == 3)
      Wrong.Symbols.push_back({"_conflict", 0x1000, 0, true});
    if (Case == 4)
      Wrong.Segments[0].Flags = SegmentFlags::Readable;
    EXPECT_FALSE(swiftMangledCGContextCGRectClassMethodSourceABI(Wrong, 0x1000))
        << Case;
  }
}

TEST(NativeSourceHints, CGPointClassTransformRequiresTheCompleteDeclaration) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  const std::string Compiler = "_$s8PointABI0A5RelayC4echoySo7CGPointVAFF";
  const std::string Generic =
      "_$s8PointABI07GenericA5RelayC4echoySo7CGPointVAFF";
  const std::string Named = "_$"
                            "s3WMF28ColumnarCollectionViewLayoutC19targetConten"
                            "tOffset011forProposedgH0So7CGPointVAG_tF";
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    Image.Arch = Architecture;
    for (const auto &Name : {Compiler, Generic, Named}) {
      Image.Symbols = {{Name, 0x1000, 0, true}};
      const auto Declaration =
          swiftMangledCGPointClassMethodDeclaration(Image, 0x1000);
      ASSERT_TRUE(Declaration) << Name;
      const auto &Hint = Declaration->Signature;
      EXPECT_EQ(Declaration->Module, Name == Named ? "WMF" : "PointABI");
      EXPECT_EQ(Declaration->ClassName, Name == Named
                                            ? "ColumnarCollectionViewLayout"
                                        : Name == Generic ? "GenericPointRelay"
                                                          : "PointRelay");
      EXPECT_EQ(Hint.Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
      EXPECT_EQ(Hint.Convention, SourceFunctionTypeHint::ConventionKind::Swift);
      EXPECT_EQ(Hint.ReturnType->Size, 16U);
      ASSERT_EQ(Hint.ReturnComponents.size(), 2U);
      ASSERT_EQ(Hint.Parameters.size(), 2U);
      ASSERT_EQ(Hint.Parameters[0].Components.size(), 2U);
      const auto &TRI = getTargetRegInfo(Architecture);
      for (size_t I = 0; I != 2; ++I) {
        EXPECT_EQ(Hint.ReturnComponents[I].RegisterOffset, TRI.FPParamRegs[I]);
        EXPECT_EQ(Hint.Parameters[0].Components[I].RegisterOffset,
                  TRI.FPParamRegs[I]);
      }
      EXPECT_EQ(Hint.Parameters[1].TheRole,
                SourceParameterTypeHint::Role::SwiftContext);
      const auto Shared =
          swiftMangledReceiverClassMethodSourceABI(Image, 0x1000);
      ASSERT_TRUE(Shared);
      EXPECT_TRUE(equalSourceABIs(*Shared, Hint));
      auto Duplicate = Image;
      Duplicate.Symbols.push_back(Duplicate.Symbols[0]);
      EXPECT_TRUE(swiftMangledCGPointClassMethodSourceABI(Duplicate, 0x1000));
    }
    Image.Symbols = {{Compiler, 0x1000, 0, true}};
    for (const auto &Name :
         {Compiler + "To", Compiler + "TA", Compiler + "Z", Compiler + "Tj",
          std::string("_$s8PointABI0A5RelayC4echoySo7CGPointVAFKF"),
          std::string("_$s8PointABI0A5RelayC4echoySo7CGPointVAFYaF"),
          std::string("_$s8PointABI0A5RelayC4echoySo7CGPointVAFzF"),
          std::string("_$s8PointABI0A5RelayV4echoySo7CGPointVAFF"),
          std::string("_$s8PointABI0A5RelayC4echoySo6CGSizeVAFF"),
          std::string("_$s8PointABI0A5RelayC4echoySo7CGPointVAFSgF"),
          std::string("_$s8PointABI0A5RelayC4echoySo7CGPointVSo6CGSizeVF"),
          std::string("_$s8PointABI0A5RelayC4echoySo7CGPointVSgAFF"),
          std::string("_$s8PointABI0A5RelayC4echoySo7CGPointV_So6CGSizeVtF")}) {
      auto Wrong = Image;
      Wrong.Symbols[0].Name = Name;
      EXPECT_FALSE(swiftMangledCGPointClassMethodSourceABI(Wrong, 0x1000))
          << Name;
    }
    for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
      auto Wrong = Image;
      if (Mutation == 0)
        Wrong.Format = BinaryFormat::ELF;
      if (Mutation == 1)
        Wrong.IsRelocatable = true;
      if (Mutation == 2)
        Wrong.Bits = Bitness::Bits32;
      if (Mutation == 3)
        Wrong.Symbols[0].IsBoundaryGuess = true;
      if (Mutation == 4)
        Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
      if (Mutation == 5)
        Wrong.Symbols[0].IsFunc = false;
      if (Mutation == 6)
        Wrong.Segments[0].Flags = SegmentFlags::Readable;
      EXPECT_FALSE(swiftMangledCGPointClassMethodSourceABI(Wrong, 0x1000))
          << Mutation;
    }
  }
}

TEST(NativeSourceHints, CGRectClassInitializerUsesFourFPLanesAndSwiftSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$s3WMF11ActionsViewC5frameACSo6CGRectV_tcfc", 0x1000, 0, true});
  const auto Hint =
      sdk::swiftMangledCGRectClassInitializerSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  ASSERT_EQ(Hint->Parameters.size(), 2U);
  ASSERT_EQ(Hint->Parameters[0].Components.size(), 4U);
  EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name = "_$s3WMF11ActionsViewC5frameACSo6CGRectV_tcfC";
  EXPECT_FALSE(sdk::swiftMangledCGRectClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name = "_$s3WMF11ActionsViewC5frameACSo6CGSizeV_tcfc";
  EXPECT_FALSE(sdk::swiftMangledCGRectClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name += "To";
  EXPECT_FALSE(sdk::swiftMangledCGRectClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(sdk::swiftMangledCGRectClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Arch = Arch::X64;
  EXPECT_FALSE(sdk::swiftMangledCGRectClassInitializerSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints, AnyClassInitializerUsesExistentialAndSwiftSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$s6Lottie20NullCompositionLayerC5layerACyp_tcfc", 0x1000, 0, true});
  const auto Hint =
      sdk::swiftMangledAnyClassInitializerSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(Hint->ReturnLocation.RegisterOffset, a64reg::X0);
  ASSERT_EQ(Hint->Parameters.size(), 2U);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(Hint->Parameters[1].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name = "_$s6Lottie20NullCompositionLayerC5layerACyp_tcfC";
  EXPECT_FALSE(sdk::swiftMangledAnyClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name =
      "_$s6Lottie20NullCompositionLayerC5layerACSo8CALayerC_tcfc";
  EXPECT_FALSE(sdk::swiftMangledAnyClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name += "To";
  EXPECT_FALSE(sdk::swiftMangledAnyClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(sdk::swiftMangledAnyClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Arch = Arch::X64;
  EXPECT_FALSE(sdk::swiftMangledAnyClassInitializerSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints, OptionalStringExtensionGetterUsesSwiftSelfAndPair) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$sSo5NSURLC7WMFDataE23wmf_languageVariantCodeSSSgvg", 0x1000, 0,
       true});
  const auto Hint =
      sdk::swiftMangledObjCOptionalStringGetterSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(Hint->ReturnType->Size, 16U);
  ASSERT_EQ(Hint->ReturnComponents.size(), 2U);
  EXPECT_EQ(Hint->ReturnComponents[0].RegisterOffset, a64reg::X0);
  EXPECT_EQ(Hint->ReturnComponents[1].RegisterOffset, a64reg::X1);
  ASSERT_EQ(Hint->Parameters.size(), 1U);
  EXPECT_EQ(Hint->Parameters[0].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name = "_$sSo5NSURLC7WMFDataE23wmf_languageVariantCodeSSvg";
  EXPECT_FALSE(
      sdk::swiftMangledObjCOptionalStringGetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name =
      "_$sSo5NSURLC7WMFDataE23wmf_languageVariantCodeSiSgvg";
  EXPECT_FALSE(
      sdk::swiftMangledObjCOptionalStringGetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name += "To";
  EXPECT_FALSE(
      sdk::swiftMangledObjCOptionalStringGetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(
      sdk::swiftMangledObjCOptionalStringGetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Arch = Arch::X64;
  EXPECT_FALSE(
      sdk::swiftMangledObjCOptionalStringGetterSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints, OptionalUInt64AndObjCClassGettersKeepTheirReturns) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$sSo10WMFArticleC3WMFE7quadKeys6UInt64VSgvg", 0x1000, 0, true});
  const auto UInt64 =
      sdk::swiftMangledObjCOptionalUInt64OrClassGetterSourceABI(Image, 0x1000);
  ASSERT_TRUE(UInt64);
  EXPECT_EQ(UInt64->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(UInt64->ReturnType->Size, 16U);
  ASSERT_EQ(UInt64->ReturnComponents.size(), 2U);
  EXPECT_EQ(UInt64->ReturnComponents[0].RegisterOffset, a64reg::X0);
  EXPECT_EQ(UInt64->ReturnComponents[1].RegisterOffset, a64reg::X1);
  ASSERT_EQ(UInt64->Parameters.size(), 1U);
  EXPECT_EQ(UInt64->Parameters[0].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(UInt64->Parameters[0].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*UInt64, Error)) << Error;

  Image.Symbols[0].Name = "_$sSo10WMFArticleC3WMFE8locationSo10CLLocationCSgvg";
  const auto Class =
      sdk::swiftMangledObjCOptionalUInt64OrClassGetterSourceABI(Image, 0x1000);
  ASSERT_TRUE(Class);
  EXPECT_EQ(Class->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(Class->ReturnType->Size, 8U);
  EXPECT_EQ(Class->ReturnLocation.RegisterOffset, a64reg::X0);
  ASSERT_EQ(Class->Parameters.size(), 1U);
  EXPECT_EQ(Class->Parameters[0].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Class->Parameters[0].Location.RegisterOffset, a64reg::X20);
  EXPECT_TRUE(validateSourceABI(*Class, Error)) << Error;

  Image.Symbols[0].Name =
      "_$s3WMF5ThemeC26searchFieldBackgroundImageSo7UIImageCSgvg";
  const auto ClassMember =
      sdk::swiftMangledObjCOptionalUInt64OrClassGetterSourceABI(Image, 0x1000);
  ASSERT_TRUE(ClassMember);
  EXPECT_EQ(ClassMember->ReturnType->Size, 8U);
  EXPECT_EQ(ClassMember->ReturnLocation.RegisterOffset, a64reg::X0);
  ASSERT_EQ(ClassMember->Parameters.size(), 1U);
  EXPECT_EQ(ClassMember->Parameters[0].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(ClassMember->Parameters[0].Location.RegisterOffset, a64reg::X20);
  EXPECT_TRUE(validateSourceABI(*ClassMember, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name += "To";
  EXPECT_FALSE(
      sdk::swiftMangledObjCOptionalUInt64OrClassGetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name = "_$sSo10WMFArticleC3WMFE8locationSo10CLLocationCvg";
  EXPECT_FALSE(
      sdk::swiftMangledObjCOptionalUInt64OrClassGetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name = "_$sSo10WMFArticleC3WMFE7quadKeySiSgvg";
  EXPECT_FALSE(
      sdk::swiftMangledObjCOptionalUInt64OrClassGetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(
      sdk::swiftMangledObjCOptionalUInt64OrClassGetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Arch = Arch::X64;
  EXPECT_FALSE(
      sdk::swiftMangledObjCOptionalUInt64OrClassGetterSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints, OptionalDictionaryExtensionGetterUsesSwiftSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back({"_$sSo14NSUserDefaultsC3WMFE31wmf_"
                           "yearToSessionSecondsMappingSDySSSiGSgvg",
                           0x1000, 0, true});
  const auto Hint =
      sdk::swiftMangledObjCOptionalStringIntDictionaryGetterSourceABI(Image,
                                                                      0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(Hint->ReturnLocation.RegisterOffset, a64reg::X0);
  ASSERT_EQ(Hint->Parameters.size(), 1U);
  EXPECT_EQ(Hint->Parameters[0].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name =
      "_$sSo14NSUserDefaultsC3WMFE31wmf_yearToSessionSecondsMappingSDySSSiGvg";
  EXPECT_FALSE(sdk::swiftMangledObjCOptionalStringIntDictionaryGetterSourceABI(
      Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name = "_$sSo14NSUserDefaultsC3WMFE31wmf_"
                          "yearToSessionSecondsMappingSDySSSbGSgvg";
  EXPECT_FALSE(sdk::swiftMangledObjCOptionalStringIntDictionaryGetterSourceABI(
      Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name += "To";
  EXPECT_FALSE(sdk::swiftMangledObjCOptionalStringIntDictionaryGetterSourceABI(
      Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(sdk::swiftMangledObjCOptionalStringIntDictionaryGetterSourceABI(
      Wrong, 0x1000));
  Wrong = Image;
  Wrong.Arch = Arch::X64;
  EXPECT_FALSE(sdk::swiftMangledObjCOptionalStringIntDictionaryGetterSourceABI(
      Wrong, 0x1000));
}

TEST(NativeSourceHints, SpecializedUIColorPairInitializerUsesSwiftSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$s3WMF8GradientC10startColor03endD0ACSo7UIColorC_AGtcfcTf4ggn_n",
       0x1000, 0, true});
  const auto Hint =
      sdk::swiftMangledUIColorPairClassInitializerSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(Hint->ReturnLocation.RegisterOffset, a64reg::X0);
  ASSERT_EQ(Hint->Parameters.size(), 3U);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, a64reg::X1);
  EXPECT_EQ(Hint->Parameters[2].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[2].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name =
      "_$s3WMF8GradientC10startColor03endD0ACSo7UIColorC_AGtcfc";
  EXPECT_FALSE(
      sdk::swiftMangledUIColorPairClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name =
      "_$s3WMF8GradientC10startColor03endD0ACSo7UIColorC_AGtcfCTf4ggn_n";
  EXPECT_FALSE(
      sdk::swiftMangledUIColorPairClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name =
      "_$s3WMF8GradientC10startColor03endD0ACSo7UIButtonC_AGtcfcTf4ggn_n";
  EXPECT_FALSE(
      sdk::swiftMangledUIColorPairClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name += "To";
  EXPECT_FALSE(
      sdk::swiftMangledUIColorPairClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(
      sdk::swiftMangledUIColorPairClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Arch = Arch::X64;
  EXPECT_FALSE(
      sdk::swiftMangledUIColorPairClassInitializerSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints, CoderClassInitializerUsesSwiftSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$s4main13TestCoderViewC5coderACSgSo7NSCoderC_tcfc", 0x1000, 0, true});
  const auto Hint =
      sdk::swiftMangledCoderClassInitializerSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Hint->ReturnLocation.RegisterOffset, a64reg::X0);
  ASSERT_EQ(Hint->Parameters.size(), 2U);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(Hint->Parameters[1].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Nonfailable = Image;
  Nonfailable.Symbols[0].Name =
      "_$s4main16NonfailableCoderC5coderACSo7NSCoderC_tcfc";
  const auto Plain =
      sdk::swiftMangledCoderClassInitializerSourceABI(Nonfailable, 0x1000);
  ASSERT_TRUE(Plain);
  EXPECT_EQ(Plain->ReturnLocation.RegisterOffset, a64reg::X0);
  EXPECT_EQ(Plain->Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(Plain->Parameters[1].Location.RegisterOffset, a64reg::X20);
  EXPECT_TRUE(validateSourceABI(*Plain, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name = "_$s4main13TestCoderViewC5coderACSgSo7NSCoderC_tcfC";
  EXPECT_FALSE(sdk::swiftMangledCoderClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name += "To";
  EXPECT_FALSE(sdk::swiftMangledCoderClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name = "_$s4main13TestCoderViewC5coderSSSo7NSCoderC_tcfc";
  EXPECT_FALSE(sdk::swiftMangledCoderClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name = "_$s4main13TestCoderViewC5coderACSgSo8NSObjectC_tcfc";
  EXPECT_FALSE(sdk::swiftMangledCoderClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(sdk::swiftMangledCoderClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Arch = Arch::X64;
  EXPECT_FALSE(sdk::swiftMangledCoderClassInitializerSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints, DeadCoderSpecializationKeepsOnlySwiftSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  const std::string Base = "_$s4main13TestCoderViewC5coderACSgSo7NSCoderC_tcfc";
  Image.Symbols.push_back({Base + "Tf4dn_n", 0x1000, 0, true});
  const auto Hint =
      sdk::swiftMangledCoderClassInitializerSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Hint->ReturnLocation.RegisterOffset, a64reg::X0);
  ASSERT_EQ(Hint->Parameters.size(), 1U);
  EXPECT_EQ(Hint->Parameters[0].Name, "self");
  EXPECT_EQ(Hint->Parameters[0].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Plain = Image;
  Plain.Symbols[0].Name =
      "_$s4main16NonfailableCoderC5coderACSo7NSCoderC_tcfcTf4dn_n";
  const auto Nonfailable =
      sdk::swiftMangledCoderClassInitializerSourceABI(Plain, 0x1000);
  ASSERT_TRUE(Nonfailable);
  ASSERT_EQ(Nonfailable->Parameters.size(), 1U);
  EXPECT_EQ(Nonfailable->Parameters[0].Location.RegisterOffset, a64reg::X20);
  EXPECT_TRUE(validateSourceABI(*Nonfailable, Error)) << Error;

  for (llvm::StringRef Suffix : {"Tf4nd_n", "Tf4dd_n", "Tf4nn_n", "Tf4gn_n",
                                 "Tf4dg_n", "Tf4d_n", "Tf3dn_n", "Tf4dn_nTo"}) {
    SCOPED_TRACE(Suffix.str());
    auto Wrong = Image;
    Wrong.Symbols[0].Name = Base + Suffix.str();
    EXPECT_FALSE(
        sdk::swiftMangledCoderClassInitializerSourceABI(Wrong, 0x1000));
  }
  for (llvm::StringRef Name :
       {"_$s4main13TestCoderViewC5coderACSgSo7NSCoderC_tcfCTf4dn_n",
        "_$s4main13TestCoderViewC5coderSSSo7NSCoderC_tcfcTf4dn_n",
        "_$s4main13TestCoderViewC5coderACSgSo8NSObjectC_tcfcTf4dn_n"}) {
    auto Wrong = Image;
    Wrong.Symbols[0].Name = Name.str();
    EXPECT_FALSE(
        sdk::swiftMangledCoderClassInitializerSourceABI(Wrong, 0x1000));
  }
  auto Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(sdk::swiftMangledCoderClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Arch = Arch::X64;
  EXPECT_FALSE(sdk::swiftMangledCoderClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.IsRelocatable = true;
  EXPECT_FALSE(sdk::swiftMangledCoderClassInitializerSourceABI(Wrong, 0x1000));

  // Re-lifting must bind the receiver's physical x20 carrier. A body that
  // instead consumes the deleted coder's x0 live-in still fails source flow.
  for (const bool UsesDeletedCoder : {false, true}) {
    SCOPED_TRACE(UsesDeletedCoder);
    LowFunc Low;
    Low.Entry = 0x1000;
    Low.Name = Image.Symbols[0].Name;
    LowBlock Block;
    Block.Id = 0;
    Block.StartAddr = 0x1000;
    Block.EndAddr = 0x1008;
    LowOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Addr = 0x1000;
    Copy.Output = NdVar::reg(a64reg::X0, 8);
    Copy.addInput(NdVar::reg(UsesDeletedCoder ? a64reg::X0 : a64reg::X20, 8));
    LowOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = 0x1004;
    Return.addInput(NdVar::reg(a64reg::X0, 8));
    Block.Ops = {Copy, Return};
    Low.Blocks.push_back(Block);
    const std::map<va_t, SourceFunctionTypeHint> Hints{{Low.Entry, *Hint}};
    LowToMedConverter Converter;
    Converter.setSourceCallHintsEnabled(true);
    Converter.setSourceEntryTypeHints(&Hints);
    auto Med = Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
    Med.SourceTypeHint = *Hint;
    inferMedTypes(Med, Arch::AArch64);
    if (UsesDeletedCoder) {
      EXPECT_FALSE(Med.SourceParametersBound);
      continue;
    }
    ASSERT_TRUE(Med.SourceParametersBound);
    const auto High = MedToHighConverter().convert(Med, Arch::AArch64);
    ASSERT_EQ(High.Params.size(), 1U);
    EXPECT_EQ(High.Params[0].Name, "self");
    const auto Flow = analyzeHighSourceFlow(High, true);
    EXPECT_TRUE(Flow.Items.empty());
  }
}

TEST(NativeSourceHints, NibBundleClassInitializerUsesSwiftSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$s4main8TestViewC7nibName6bundleACSSSg_So8NSBundleCSgtcfc", 0x1000, 0,
       true});
  const auto Hint =
      sdk::swiftMangledNibBundleClassInitializerSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Hint->ReturnLocation.RegisterOffset, a64reg::X0);
  ASSERT_EQ(Hint->Parameters.size(), 4U);
  for (unsigned I = 0; I != 3; ++I)
    EXPECT_EQ(Hint->Parameters[I].Location.RegisterOffset, a64reg::X0 + 8 * I);
  EXPECT_EQ(Hint->Parameters[0].Type->Kind, NdTypeKind::Int);
  EXPECT_EQ(Hint->Parameters[1].Type->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Hint->Parameters[2].Type->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Hint->Parameters[3].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[3].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name =
      "_$s4main8TestViewC7nibName6bundleACSSSg_So8NSBundleCSgtcfC";
  EXPECT_FALSE(
      sdk::swiftMangledNibBundleClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name += "To";
  EXPECT_FALSE(
      sdk::swiftMangledNibBundleClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name =
      "_$s4main8TestViewC7nibName6bundleACSS_So8NSBundleCSgtcfc";
  EXPECT_FALSE(
      sdk::swiftMangledNibBundleClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name =
      "_$s4main8TestViewC7nibName6bundleACSSSg_So8NSObjectCSgtcfc";
  EXPECT_FALSE(
      sdk::swiftMangledNibBundleClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(
      sdk::swiftMangledNibBundleClassInitializerSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Arch = Arch::X64;
  EXPECT_FALSE(
      sdk::swiftMangledNibBundleClassInitializerSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints, BundleModuleClosureRequiresContextFreeBody) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  constexpr const char *Name = "_$sSo8NSBundleC7TestPkgE6moduleABvpZfiAByXEfU_";
  Image.Symbols.push_back({Name, 0x1000, 0, true});
  HighFunc Function;
  Function.Entry = 0x1000;
  Function.Name = Name;
  Function.ReturnType = NdType::makePtr(NdType::makeVoid());
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeConst(0, 8);
  Return.RetVal->Type = Function.ReturnType;
  Function.Body.push_back(Return);
  const auto Hint =
      sdk::swiftMangledBundleModuleClosureSourceABI(Image, 0x1000, Function);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_TRUE(Hint->Parameters.empty());
  EXPECT_EQ(Hint->ReturnLocation.RegisterOffset, a64reg::X0);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name = "_$sSo8NSObjectC7TestPkgE6moduleABvpZfiAByXEfU_";
  EXPECT_FALSE(
      sdk::swiftMangledBundleModuleClosureSourceABI(Wrong, 0x1000, Function));
  Wrong = Image;
  Wrong.Symbols[0].Name = "_$sSo8NSBundleC7TestPkgE5otherABvpZfiAByXEfU_";
  EXPECT_FALSE(
      sdk::swiftMangledBundleModuleClosureSourceABI(Wrong, 0x1000, Function));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(
      sdk::swiftMangledBundleModuleClosureSourceABI(Wrong, 0x1000, Function));
  Wrong = Image;
  Wrong.Arch = Arch::X64;
  EXPECT_FALSE(
      sdk::swiftMangledBundleModuleClosureSourceABI(Wrong, 0x1000, Function));

  auto Capturing = Function;
  MedVar Context;
  Context.Kind = MedVar::Reg;
  Context.Id = 20;
  Context.RegOff = a64reg::X20;
  Context.Size = 8;
  Capturing.Body[0].RetVal =
      HighExpr::makeVar(Context, NdType::makePtr(NdType::makeVoid()));
  EXPECT_FALSE(
      sdk::swiftMangledBundleModuleClosureSourceABI(Image, 0x1000, Capturing));
  Capturing = Function;
  Capturing.Body.clear();
  EXPECT_FALSE(
      sdk::swiftMangledBundleModuleClosureSourceABI(Image, 0x1000, Capturing));
  Capturing = Function;
  Capturing.Body[0].RetVal = HighExpr::makeUndef(8);
  EXPECT_FALSE(
      sdk::swiftMangledBundleModuleClosureSourceABI(Image, 0x1000, Capturing));
}

namespace {
struct NativeFixture {
  BinaryImage Image;
  MedFunc Med;
  HighFunc High;
  PipelineFunctionAudit Audit;

  explicit NativeFixture(Arch Architecture = Arch::AArch64) {
    Image.Arch = Architecture;
    Image.Format = BinaryFormat::MachO;
    Image.Bits = Bitness::Bits64;
    Segment Text;
    Text.VA = 0x1000;
    Text.Size = Text.FileSz = 0x100;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(0x100);
    Image.Segments.push_back(std::move(Text));
    const auto &TRI = getTargetRegInfo(Architecture);
    Med.Entry = High.Entry = Audit.Entry = 0x1000;
    Med.ReturnType = High.ReturnType = NdType::makeInt(4);
    Med.Blocks.emplace_back();
    Med.Blocks[0].Id = 0;
    for (unsigned Index = 0; Index < 2; ++Index) {
      MedVar Parameter;
      Parameter.Kind = MedVar::Param;
      Parameter.Id = Index;
      Parameter.RegOff = TRI.IntParamRegs[Index];
      Parameter.Size = 4;
      Parameter.TheArch = Architecture;
      Med.Params.push_back(Parameter);
      Med.TypedParams.push_back(
          {"arg" + std::to_string(Index), NdType::makeInt(4)});
      High.Params.push_back(
          {"arg" + std::to_string(Index), NdType::makeInt(4)});
    }
    MedOp Add;
    Add.Opcode = NdOp::INT_ADD;
    Add.Output.Kind = MedVar::Reg;
    Add.Output.Id = 10;
    Add.Output.SSAVer = 1;
    Add.Output.TheArch = Architecture;
    Add.Output.RegOff = TRI.IntReturnReg;
    Add.Output.Size = 4;
    Add.addInput(Med.Params[0]);
    Add.addInput(Med.Params[1]);
    Med.Blocks[0].Ops.push_back(Add);
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Med.Blocks[0].Ops.push_back(Return);
    HighStmt Statement;
    Statement.Kind = StmtKind::Return;
    Statement.RetVal = HighExpr::makeConst(42, 4);
    High.Body.push_back(std::move(Statement));
    Audit.Disposition = PipelineFunctionDisposition::Accepted;
    Audit.HasLowIR = Audit.HasMedIR = Audit.MedIRVerified = true;
    Audit.DecodedInstructions = Audit.LiftedInstructions = 2;
  }

  std::optional<SourceFunctionTypeHint> infer(std::string &Error) const {
    return inferNativeSourceTypeHint(Image, Med, High, Audit, Error);
  }
};

NativeFixture nativePairFixture(Arch Architecture) {
  NativeFixture F(Architecture);
  const auto &TRI = getTargetRegInfo(Architecture);
  F.Med.ReturnType = F.High.ReturnType = NdType::makeInt(8, false);
  for (size_t I = 0; I < F.Med.Params.size(); ++I) {
    F.Med.Params[I].Size = 8;
    F.Med.TypedParams[I].Type = F.High.Params[I].Type = NdType::makeInt(8);
  }
  F.Med.Params[1].Id = -1;
  auto &Primary = F.Med.Blocks[0].Ops[0];
  Primary.Output.Size = 8;
  Primary.Inputs[0] = F.Med.Params[0];
  Primary.Inputs[1] = MedVar::makeConst(17, 8);
  auto Secondary = Primary;
  Secondary.Opcode = NdOp::INT_XOR;
  Secondary.Output.RegOff = TRI.IntReturnRegs[1];
  Secondary.Output.Id = 11;
  Secondary.Inputs[1] = MedVar::makeConst(UINT64_C(0xfedcba9876543210), 8);
  F.Med.Blocks[0].Ops.insert(F.Med.Blocks[0].Ops.begin() + 1, Secondary);
  return F;
}

TEST(NativeSourceHints,
     IntegerPairDemandIncludesNarrowReadsButStopsAtClobbers) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
      SCOPED_TRACE(Mutation);
      const auto &TRI = getTargetRegInfo(Architecture);
      LowFunc F;
      F.Blocks.emplace_back();
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(NdVar::cst(0x1080, 8));
      LowOp Read;
      Read.Opcode = NdOp::COPY;
      Read.Output = NdVar::reg(TRI.IntReturnRegs[0], 8);
      Read.addInput(NdVar::reg(TRI.IntReturnRegs[1], 8));
      if (Mutation == 1)
        Read.Inputs[0].Size = 4;
      if (Mutation == 2)
        Call.Opcode = NdOp::INDIR_CALL;
      F.Blocks[0].Ops = {Call, Read};
      if (Mutation >= 3 && Mutation <= 5) {
        LowOp Stop;
        Stop.Opcode = Mutation == 3 ? NdOp::INTRINSIC : NdOp::COPY;
        Stop.Output = NdVar::reg(TRI.IntReturnRegs[1], Mutation == 4 ? 4 : 8);
        Stop.addInput(NdVar::cst(0, 8));
        F.Blocks[0].Ops.insert(F.Blocks[0].Ops.begin() + 1, Stop);
      }
      if (Mutation == 6) {
        F.Blocks[0].Ops.back().Opcode = NdOp::INT_XOR;
        F.Blocks[0].Ops.back().addInput(Read.Inputs[0]);
      }
      if (Mutation == 7) {
        F.Blocks[0].Ops.pop_back();
        F.Blocks.emplace_back();
        F.Blocks.back().Ops.push_back(Read);
      }
      EXPECT_EQ(observedNativeIntegerPairReturns(F, Architecture),
                Mutation <= 1 ? std::set<va_t>{0x1080} : std::set<va_t>{});
    }
}

TEST(NativeSourceHints, FourWordDemandRequiresOneCompleteCallObservation) {
  const auto &Regs = getTargetRegInfo(Arch::AArch64).IntParamRegs;
  for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    LowFunc F;
    F.Blocks.emplace_back();
    LowOp Call;
    Call.Opcode = NdOp::CALL;
    Call.addInput(NdVar::cst(0x1080, 8));
    F.Blocks[0].Ops.push_back(Call);
    for (unsigned I = 0; I < 4; ++I) {
      LowOp Read;
      Read.Opcode = NdOp::COPY;
      Read.Output = NdVar::reg(a64reg::X19 + I * 8, 8);
      Read.addInput(NdVar::reg(Regs[I], 8));
      F.Blocks[0].Ops.push_back(Read);
    }
    if (Mutation == 1)
      F.Blocks[0].Ops.back().Inputs[0].Size = 4;
    if (Mutation == 2)
      F.Blocks[0].Ops.front().Opcode = NdOp::INDIR_CALL;
    if (Mutation >= 3 && Mutation <= 5) {
      LowOp Stop;
      Stop.Opcode = Mutation == 3 ? NdOp::INTRINSIC : NdOp::COPY;
      Stop.Output = NdVar::reg(Regs[3] + (Mutation == 5 ? 4 : 0), 4);
      Stop.addInput(NdVar::cst(0, 4));
      F.Blocks[0].Ops.insert(F.Blocks[0].Ops.end() - 1, Stop);
    }
    if (Mutation == 6) {
      auto &Read = F.Blocks[0].Ops.back();
      Read.Opcode = NdOp::INT_XOR;
      Read.addInput(Read.Inputs[0]);
    }
    if (Mutation == 7) {
      const auto Last = F.Blocks[0].Ops.back();
      F.Blocks[0].Ops.pop_back();
      F.Blocks.emplace_back();
      F.Blocks.back().Ops.push_back(Last);
    }
    if (Mutation == 8)
      // Seeing the same target again must not combine separate invocations.
      F.Blocks[0].Ops.insert(F.Blocks[0].Ops.end() - 1, Call);
    if (Mutation == 9 || Mutation == 10) {
      LowOp Stop;
      Stop.Opcode = Mutation == 9 ? NdOp::RETURN : NdOp::INDIR_BR;
      F.Blocks[0].Ops.insert(F.Blocks[0].Ops.end() - 1, Stop);
    }
    if (Mutation == 11) {
      // Once copied, a later clobber of the source does not lose that word.
      LowOp Write;
      Write.Opcode = NdOp::COPY;
      Write.Output = NdVar::reg(Regs[0], 8);
      Write.addInput(NdVar::cst(0, 8));
      F.Blocks[0].Ops.insert(F.Blocks[0].Ops.begin() + 2, Write);
    }
    EXPECT_EQ(observedNativeFourWordReturns(F, Arch::AArch64),
              (Mutation == 0 || Mutation == 11) ? std::set<va_t>{0x1080}
                                                : std::set<va_t>{});
    EXPECT_TRUE(observedNativeFourWordReturns(F, Arch::X64).empty());
  }
}

struct NativeFourWordFixture : NativeFixture {
  LowFunc Low;
  SourceFunctionTypeHint Scalar;

  NativeFourWordFixture() {
    Scalar.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Scalar.ReturnType = NdType::makeInt(8, false);
    Scalar.Parameters = {{"flag", NdType::makeInt(4, false)},
                         {"first", NdType::makeInt(8, false)},
                         {"second", NdType::makeInt(8, false)},
                         {"storage", NdType::makePtr(NdType::makeVoid())}};
    std::string Error;
    EXPECT_TRUE(assignDarwinScalarSourceABI(Scalar, Arch::AArch64, Error))
        << Error;
    Low.Entry = Med.Entry;
    Low.Name = "native_four_words";
    Low.DecodedInstructionCount = Low.LiftedInstructionCount = 2;
    LowBlock Block;
    Block.Id = 0;
    Block.StartAddr = Low.Entry;
    Block.EndAddr = Low.Entry + 8;
    LowOp Mask;
    Mask.Opcode = NdOp::INT_AND;
    Mask.Addr = Low.Entry;
    Mask.Output = NdVar::tmp(0, 4);
    Mask.addInput(NdVar::reg(a64reg::X0, 4));
    Mask.addInput(NdVar::cst(1, 4));
    LowOp Extend;
    Extend.Opcode = NdOp::INT_ZEXT;
    Extend.Addr = Low.Entry;
    Extend.Seq = 1;
    Extend.Output = NdVar::reg(a64reg::X0, 8);
    Extend.addInput(Mask.Output);
    LowOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Low.Entry + 4;
    Return.addInput(NdVar::reg(a64reg::X30, 8));
    Block.Ops = {Mask, Extend, Return};
    Low.Blocks = {Block};
    bind(Scalar);
  }

  void bind(const SourceFunctionTypeHint &Hint) {
    std::map<va_t, SourceFunctionTypeHint> Hints{{Low.Entry, Hint}};
    LowToMedConverter Converter;
    Converter.setSourceCallHintsEnabled(true);
    Converter.setSourceCalleeTypeHints(&Hints);
    Converter.setSourceEntryTypeHints(&Hints);
    Med = Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
    Med.SourceTypeHint = Hint;
    recoverCallAbi(Med, Arch::AArch64, {});
    inferMedTypes(Med, Arch::AArch64);
    High = MedToHighConverter().convert(Med, Arch::AArch64);
  }

  std::optional<SourceFunctionTypeHint> refine() const {
    return refineNativeFourWordReturnHint(Image, Low, Med, High, Audit);
  }
};

TEST(NativeSourceHints, FourWordLeafRefinementReliftsAllForwardedInputs) {
  NativeFourWordFixture F;
  const auto Hint = F.refine();
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::NativeAnalysis);
  ASSERT_EQ(Hint->ReturnComponents.size(), 4U);
  const auto &Regs = getTargetRegInfo(Arch::AArch64).IntParamRegs;
  for (unsigned I = 0; I < 4; ++I) {
    EXPECT_EQ(Hint->ReturnComponents[I].RegisterOffset, Regs[I]);
    EXPECT_EQ(Hint->ReturnComponents[I].ValueBytes, 8U);
    EXPECT_TRUE(equalSourceTypes(Hint->Parameters[I].Type,
                                 F.Scalar.Parameters[I].Type));
  }
  F.bind(*Hint);
  ASSERT_TRUE(F.Med.SourceParametersBound);
  EXPECT_TRUE(equalSourceTypes(F.High.ReturnType, Hint->ReturnType));
  const auto Limitation = sdk::sourceBodyLimitation(F.High, *Hint, &F.Audit);
  EXPECT_TRUE(Limitation.empty()) << Limitation;
  EXPECT_FALSE(F.refine());
  unsigned Returns = 0;
  walkStmts(F.High.Body, [&](const HighStmt &S) {
    if (S.Kind != StmtKind::Return)
      return;
    ++Returns;
    ASSERT_TRUE(S.RetVal);
    EXPECT_TRUE(equalSourceTypes(S.RetVal->Type, Hint->ReturnType));
  });
  EXPECT_EQ(Returns, 1U);
}

TEST(NativeSourceHints, FourWordLeafRestoresUnobservedForwardedInputs) {
  NativeFourWordFixture F;
  auto Sparse = F.Scalar;
  Sparse.ReturnType = NdType::makeInt(4, false);
  Sparse.ReturnLocation.ValueBytes = 4;
  Sparse.Parameters = {F.Scalar.Parameters[0], F.Scalar.Parameters[3]};
  std::string Error;
  ASSERT_TRUE(validateSourceABI(Sparse, Error)) << Error;
  F.bind(Sparse);
  const auto Hint = F.refine();
  ASSERT_TRUE(Hint);
  ASSERT_EQ(Hint->Parameters.size(), 4U);
  ASSERT_EQ(Hint->ReturnComponents.size(), 4U);
  EXPECT_EQ(Hint->ReturnType->Fields[0]->Size, 8U);
  EXPECT_TRUE(
      equalSourceTypes(Hint->Parameters[3].Type, Sparse.Parameters[1].Type));
  F.bind(*Hint);
  const auto Limitation = sdk::sourceBodyLimitation(F.High, *Hint, &F.Audit);
  EXPECT_TRUE(Limitation.empty()) << Limitation;
  EXPECT_FALSE(F.refine());
}

TEST(NativeSourceHints, FourWordLeafRejectsIncompleteOrChangedEvidence) {
  for (unsigned Mutation = 0; Mutation < 22; ++Mutation) {
    SCOPED_TRACE(Mutation);
    NativeFourWordFixture F;
    switch (Mutation) {
    case 0:
      F.Image.Arch = Arch::X64;
      break;
    case 1:
      F.Image.IsRelocatable = true;
      break;
    case 2:
      F.Low.Entry += 4;
      break;
    case 3:
      F.Audit.Entry += 4;
      break;
    case 4:
      F.Med.SourceParametersBound = false;
      break;
    case 5:
      F.High.SourceTypeHint->ReturnType = NdType::makeInt(4);
      break;
    case 6:
      F.Med.SourceTypeHint->Origin = F.High.SourceTypeHint->Origin =
          SourceFunctionTypeHint::OriginKind::DarwinSDK;
      break;
    case 7:
      F.Low.UnsupportedInstructionAddresses.push_back(F.Low.Entry);
      break;
    case 8:
      --F.Low.LiftedInstructionCount;
      break;
    case 9:
      F.High.StructuredExceptionRegions = 1;
      break;
    case 10:
      F.Med.Blocks[0].ExceptionalSuccs.emplace_back();
      break;
    case 11:
      F.Med.Blocks[0].Ops.clear();
      break;
    case 12:
    case 13:
    case 14:
    case 15:
    case 16:
    case 17: {
      LowOp Write;
      Write.Opcode = NdOp::COPY;
      Write.Output = NdVar::reg(a64reg::X1 + (Mutation - 12) * 4, 4);
      Write.addInput(NdVar::cst(0, 4));
      F.Low.Blocks[0].Ops.insert(F.Low.Blocks[0].Ops.begin(), Write);
      break;
    }
    case 18:
    case 19: {
      LowOp Call;
      Call.Opcode = Mutation == 18 ? NdOp::CALL : NdOp::INDIR_CALL;
      Call.addInput(NdVar::cst(0x1080, 8));
      F.Low.Blocks[0].Ops.insert(F.Low.Blocks[0].Ops.begin(), Call);
      break;
    }
    case 20:
      F.Low.Blocks[0].Ops[0].Output = NdVar::reg(a64reg::X19, 8);
      break;
    case 21:
      F.Low.Blocks[0].Ops.erase(F.Low.Blocks[0].Ops.begin() + 1);
      F.bind(F.Scalar);
      break;
    }
    EXPECT_FALSE(F.refine());
  }
}

TEST(NativeSourceHints, IntegerPairsRequireBothCompleteReturnCarriers) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto F = nativePairFixture(Architecture);
      auto &Ops = F.Med.Blocks[0].Ops;
      if (Mutation == 1)
        Ops[1].Output.Size = 4;
      if (Mutation == 2)
        Ops.erase(Ops.begin() + 1);
      if (Mutation == 3)
        F.Med.Blocks[0].Preds = {99};
      if (Mutation == 4)
        F.Med.Blocks[0].ExceptionalSuccs.emplace_back();
      std::string Error;
      const auto Pair = inferNativeSourceTypeHint(
          F.Image, F.Med, F.High, F.Audit, Error, nullptr, true);
      if (Mutation) {
        EXPECT_TRUE(!Pair || Pair->ReturnType->Kind != NdTypeKind::Struct);
      } else {
        ASSERT_TRUE(Pair) << Error;
        EXPECT_EQ(Pair->ReturnType->Kind, NdTypeKind::Struct);
        EXPECT_EQ(Pair->ReturnType->Size, 16U);
        ASSERT_EQ(Pair->ReturnComponents.size(), 2U);
        for (unsigned I = 0; I < 2; ++I) {
          EXPECT_EQ(Pair->ReturnComponents[I].RegisterOffset,
                    getTargetRegInfo(Architecture).IntReturnRegs[I]);
          EXPECT_EQ(Pair->ReturnComponents[I].ValueBytes, 8U);
        }
        const auto Scalar = F.infer(Error);
        ASSERT_TRUE(Scalar) << Error;
        EXPECT_EQ(Scalar->ReturnType->Kind, NdTypeKind::Int);
      }
    }
}

TEST(NativeSourceHints,
     IntegerPairReturnsAllowScalarBitReverseAndSeparateTrapPaths) {
  auto Fixture = nativePairFixture(Arch::AArch64);
  auto &Entry = Fixture.Med.Blocks[0];
  Entry.Ops.pop_back();
  Entry.Succs = {1, 2};

  MedOp Reverse;
  Reverse.Opcode = NdOp::INTRINSIC;
  Reverse.Output.Kind = MedVar::Temp;
  Reverse.Output.Id = 300;
  Reverse.Output.Size = 8;
  Reverse.addInput(
      MedVar::makeConst(static_cast<uint64_t>(Intrinsic::A64_Rbit), 2));
  Reverse.addInput(MedVar::makeConst(1, 8));
  Entry.Ops.push_back(Reverse);

  MedBlock Returning;
  Returning.Id = 1;
  Returning.Preds = {0};
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Returning.Ops.push_back(Return);
  Fixture.Med.Blocks.push_back(Returning);

  MedBlock Trapping;
  Trapping.Id = 2;
  Trapping.Preds = {0};
  MedOp Trap;
  Trap.Opcode = NdOp::INTRINSIC;
  Trap.addInput(MedVar::makeConst(static_cast<uint64_t>(Intrinsic::Brk), 2));
  Trapping.Ops.push_back(Trap);
  Fixture.Med.Blocks.push_back(Trapping);

  std::string Error;
  const auto Pair =
      inferNativeSourceTypeHint(Fixture.Image, Fixture.Med, Fixture.High,
                                Fixture.Audit, Error, nullptr, true);
  ASSERT_TRUE(Pair) << Error;
  EXPECT_EQ(Pair->ReturnType->Kind, NdTypeKind::Struct);
  ASSERT_EQ(Pair->ReturnComponents.size(), 2U);

  Fixture.Med.Blocks[0].Ops.back().Inputs[0].ConstVal =
      static_cast<uint64_t>(Intrinsic::A64_Rev64);
  EXPECT_FALSE(inferNativeSourceTypeHint(Fixture.Image, Fixture.Med,
                                         Fixture.High, Fixture.Audit, Error,
                                         nullptr, true));
}

TEST(NativeSourceHints,
     IntegerPairRefinementRequiresTheInferredNativeIdentity) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      auto F = nativePairFixture(Architecture);
      std::string Error;
      const auto Scalar = F.infer(Error);
      ASSERT_TRUE(Scalar) << Error;
      F.Med.SourceTypeHint = F.High.SourceTypeHint = *Scalar;
      F.Med.SourceParametersBound = true;
      if (Mutation == 1)
        F.Med.SourceTypeHint->Origin = F.High.SourceTypeHint->Origin =
            SourceFunctionTypeHint::OriginKind::DarwinSDK;
      if (Mutation == 2)
        F.Audit.Entry += 4;
      if (Mutation == 3)
        F.Med.SourceParametersBound = false;
      if (Mutation == 4)
        F.High.SourceTypeHint->ReturnType = NdType::makeInt(4);
      if (Mutation == 5)
        F.Med.Blocks[0].Ops[1].Output.Size = 4;
      const auto Pair =
          refineNativeIntegerPairReturnHint(F.Med, F.High, F.Audit);
      EXPECT_EQ(bool(Pair), Mutation == 0);
      if (Pair) {
        F.Med.SourceTypeHint = F.High.SourceTypeHint = *Pair;
        F.Med.ReturnType = F.High.ReturnType = Pair->ReturnType;
        EXPECT_FALSE(refineNativeIntegerPairReturnHint(F.Med, F.High, F.Audit));
      }
    }
}

TEST(NativeSourceHints, RefinementNarrowsIntegerParametersWithOnlyLowWordUses) {
  const auto Local = [](int Id, uint16_t Size) {
    MedVar Value;
    Value.Kind = MedVar::Temp;
    Value.Id = Id;
    Value.Size = Size;
    return HighExpr::makeVar(Value, NdType::makeInt(Size, false));
  };
  const auto Assign = [](ExprPtr Destination, ExprPtr Value) {
    HighStmt Statement;
    Statement.Kind = StmtKind::Assign;
    Statement.Dst = std::move(Destination);
    Statement.Val = std::move(Value);
    return Statement;
  };
  const auto Returning = [](ExprPtr Value) {
    HighStmt Statement;
    Statement.Kind = StmtKind::Return;
    Statement.RetVal = std::move(Value);
    return Statement;
  };
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 3; ++Mutation) {
      NativeFixture Fixture(Architecture);
      for (size_t I = 0; I < Fixture.High.Params.size(); ++I) {
        Fixture.Med.Params[I].Size = 8;
        Fixture.Med.TypedParams[I].Type = NdType::makeInt(8, false);
        Fixture.High.Params[I].Type = NdType::makeInt(8, false);
      }
      SourceFunctionTypeHint Original;
      Original.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      Original.ReturnType = Fixture.High.ReturnType;
      const auto Pointer = NdType::makePtr(NdType::makeVoid());
      Fixture.High.Params[0].Type = Pointer;
      Original.Parameters = {{"native_arg0", Pointer},
                             {"native_arg1", NdType::makeInt(8, false)}};
      std::string Error;
      ASSERT_TRUE(assignDarwinScalarSourceABI(Original, Architecture, Error))
          << Error;
      Fixture.High.SourceTypeHint = Original;

      auto Parameter =
          HighExpr::makeVar(Fixture.Med.Params[1], NdType::makeInt(8, false));
      auto Prefix =
          HighExpr::makeBinop(NdOp::SUBBYTES, Parameter,
                              HighExpr::makeConst(Mutation == 2 ? 1 : 0, 4));
      Prefix->Type = NdType::makeInt(4, false);
      auto PointerParameter = HighExpr::makeVar(Fixture.Med.Params[0], Pointer);
      auto PointerLocal = Local(40, 8);
      PointerLocal->Type = Pointer;
      Fixture.High.Body = {Assign(PointerLocal, PointerParameter),
                           Assign(Local(41, 4), Prefix),
                           Returning(HighExpr::makeConst(0, 4))};
      if (Mutation == 1)
        Fixture.High.Body.insert(Fixture.High.Body.begin() + 2,
                                 Assign(Local(42, 8), Parameter));

      const auto Refined =
          refineNativeSourceTypeHint(Fixture.High, Fixture.Audit);
      EXPECT_EQ(bool(Refined), Mutation == 0) << Mutation;
      if (Refined) {
        ASSERT_EQ(Refined->Parameters.size(), 2U);
        EXPECT_EQ(Refined->Parameters[0].Type->Size, 8U);
        EXPECT_EQ(Refined->Parameters[1].Type->Size, 4U);
        EXPECT_EQ(Refined->Parameters[1].Location.ValueBytes, 4U);
        EXPECT_EQ(Refined->Parameters[1].Location.RegisterOffset,
                  getTargetRegInfo(Architecture).IntParamRegs[1]);
      }
    }
}

TEST(NativeSourceHints, RefinesOnlyExplicitlyPartialIntegerReturnCandidates) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
      SCOPED_TRACE(Mutation);
      NativeFixture F(Architecture);
      SourceFunctionTypeHint Original;
      Original.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      Original.ReturnType = NdType::makeInt(8, false);
      Original.Parameters = {{"arg0", NdType::makeInt(4)},
                             {"arg1", NdType::makeInt(4)}};
      std::string Error;
      ASSERT_TRUE(assignDarwinScalarSourceABI(Original, Architecture, Error));
      F.High.ReturnType = Original.ReturnType;
      F.High.SourceTypeHint = Original;
      MedVar Local;
      Local.Kind = MedVar::Temp;
      Local.Id = 40;
      Local.Size = 8;
      const auto LocalExpr = [&] {
        return HighExpr::makeVar(Local, NdType::makeInt(8, false));
      };
      auto Low = HighExpr::makeVar(F.Med.Params[0], NdType::makeInt(4));
      if (Mutation == 1)
        Low = HighExpr::makeUndef(4);
      auto Upper = HighExpr::makeUndef(4);
      if (Mutation == 2)
        Upper = HighExpr::makeConst(0, 4);
      auto Partial = HighExpr::makeBinop(NdOp::CONCAT, Upper, Low);
      Partial->Type = NdType::makeInt(8, false);
      HighStmt Define;
      Define.Kind = StmtKind::Assign;
      Define.Dst = LocalExpr();
      Define.Val = Partial;
      HighStmt Otherwise = Define;
      Otherwise.Dst = LocalExpr();
      Otherwise.Val = HighExpr::makeConst(0, 8);
      HighStmt Branch;
      Branch.Kind = StmtKind::IfElse;
      Branch.Cond = HighExpr::makeVar(F.Med.Params[1], NdType::makeInt(4));
      Branch.Body = {Define};
      Branch.ElseBody = {Otherwise};
      if (Mutation == 3)
        Branch.ElseBody.clear();
      if (Mutation == 4)
        Branch.ElseBody[0].Val = LocalExpr(); // Cyclic/missing definition.
      HighStmt Return;
      Return.Kind = StmtKind::Return;
      Return.RetVal = LocalExpr();
      F.High.Body = {Branch, Return};
      if (Mutation == 5)
        F.High.SourceTypeHint->Origin =
            SourceFunctionTypeHint::OriginKind::ObjCRuntime;
      if (Mutation == 6)
        ++F.Audit.Entry;
      if (Mutation == 7)
        F.High.Body.back().RetVal = HighExpr::makeUndef(8);
      if (Mutation == 8)
        F.High.StructuredExceptionRegions = 1;
      if (Mutation == 9)
        Partial->Operands[1]->Type = NdType::makeInt(2);
      if (Mutation == 0) {
        const auto Flow = analyzeHighSourceFlow(F.High, true);
        for (const auto &Item : Flow.Items)
          ADD_FAILURE() << Item.Reason;
      }
      const auto Refined = refineNativeSourceTypeHint(F.High, F.Audit);
      EXPECT_EQ(bool(Refined), Mutation == 0);
      if (Refined) {
        EXPECT_EQ(Refined->ReturnType->Size, 4U);
        EXPECT_EQ(Refined->ReturnLocation.ValueBytes, 4U);
        EXPECT_EQ(Refined->Parameters.size(), Original.Parameters.size());
        EXPECT_EQ(Refined->ReturnLocation.RegisterOffset,
                  Original.ReturnLocation.RegisterOffset);
        // The candidate is not a mutation of the analyzed body or its ABI.
        EXPECT_EQ(F.High.SourceTypeHint->ReturnType->Size, 8U);
        EXPECT_EQ(F.High.Body.back().RetVal->Type->Size, 8U);
      }
    }
}

TEST(NativeSourceHints,
     RefinesLoweredSignedCallResultWithoutInventingUpperBits) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Signed : {false, true}) {
      SCOPED_TRACE(int(Architecture));
      SCOPED_TRACE(Signed);
      NativeFixture F(Architecture);
      const auto &TRI = getTargetRegInfo(Architecture);
      SourceFunctionTypeHint Callee, Entry;
      Callee.Origin = Entry.Origin =
          SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      Callee.ReturnType = NdType::makeInt(4, Signed);
      Entry.ReturnType = NdType::makeInt(8, false);
      std::string Error;
      ASSERT_TRUE(assignDarwinScalarSourceABI(Callee, Architecture, Error));
      ASSERT_TRUE(assignDarwinScalarSourceABI(Entry, Architecture, Error));
      std::map<va_t, SourceFunctionTypeHint> Hints{{0x1000, Entry},
                                                   {0x1080, Callee}};
      LowFunc Low;
      Low.Entry = 0x1000;
      LowBlock B;
      B.Id = 0;
      B.StartAddr = 0x1000;
      B.EndAddr = 0x1008;
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.Addr = 0x1000;
      Call.Output = NdVar::reg(TRI.IntReturnReg, 8);
      Call.addInput(NdVar::cst(0x1080, 8));
      LowOp Return;
      Return.Opcode = NdOp::RETURN;
      Return.Addr = 0x1004;
      Return.addInput(NdVar::reg(TRI.IntReturnReg, 8));
      B.Ops = {Call, Return};
      Low.Blocks = {B};
      const auto Lower = [&] {
        LowToMedConverter Converter;
        Converter.setSourceCallHintsEnabled(true);
        Converter.setSourceCalleeTypeHints(&Hints);
        Converter.setSourceEntryTypeHints(&Hints);
        F.Med = Converter.convert(Low, Architecture, BinaryFormat::MachO);
        recoverCallAbi(F.Med, Architecture, {{0x1080, "native_prefix"}});
        F.Med.SourceTypeHint = Hints.at(0x1000);
        inferMedTypes(F.Med, Architecture);
        F.High = MedToHighConverter().convert(F.Med, Architecture);
      };
      Lower();
      ASSERT_EQ(F.High.ReturnType->Size, 8U);
      const auto Allowed = [](const HighExpr &) { return true; };
      EXPECT_FALSE(
          sdk::sourceBodyLimitation(F.High, Entry, &F.Audit, Allowed).empty());
      const auto Refined = refineNativeSourceTypeHint(F.High, F.Audit);
      ASSERT_TRUE(Refined);
      EXPECT_EQ(Refined->ReturnType->Size, 4U);
      EXPECT_EQ(Refined->ReturnLocation.ValueBytes, 4U);
      EXPECT_EQ(F.High.ReturnType->Size, 8U);
      EXPECT_EQ(Hints.at(0x1080).ReturnType->IsSigned, Signed);
      Hints[0x1000] = *Refined;
      Lower();
      EXPECT_TRUE(sdk::sourceBodyLimitation(F.High, *Refined, &F.Audit, Allowed)
                      .empty());
      // The candidate does not define the original caller's high word.
      Hints[0x1000] = Entry;
      Lower();
      EXPECT_FALSE(
          sdk::sourceBodyLimitation(F.High, Entry, &F.Audit, Allowed).empty());
    }
}

TEST(NativeSourceHints, IntegerPrefixCallBitsRequireTheCompleteDeclaredResult) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 17; ++Mutation) {
      SCOPED_TRACE(int(Architecture));
      SCOPED_TRACE(Mutation);
      NativeFixture F(Architecture);
      SourceFunctionTypeHint Entry, Callee;
      Entry.Origin = Callee.Origin =
          SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      Entry.ReturnType = NdType::makeInt(8, false);
      for (const auto &Parameter : F.High.Params)
        Entry.Parameters.push_back({Parameter.Name, Parameter.Type});
      Callee.ReturnType = NdType::makeInt(4, true);
      Callee.Parameters = {{"input", NdType::makeInt(4)}};
      std::string Error;
      ASSERT_TRUE(assignDarwinScalarSourceABI(Entry, Architecture, Error));
      ASSERT_TRUE(assignDarwinScalarSourceABI(Callee, Architecture, Error));
      auto Hint = std::make_shared<SourceCallTypeHint>();
      Hint->Signature = Callee;
      auto Call = HighExpr::makeCall(
          "native_prefix", 0x1080,
          {HighExpr::makeVar(F.Med.Params[0], NdType::makeInt(4))});
      Call->SourceCallHint = Hint;
      Call->Type = sourceABICallResultType(Callee);
      MedVar Local;
      Local.Kind = MedVar::Temp;
      Local.Id = 40;
      Local.Size = 4;
      const auto Value = [&] {
        return HighExpr::makeVar(Local, Callee.ReturnType);
      };
      HighStmt Define;
      Define.Kind = StmtKind::Assign;
      Define.Dst = Value();
      Define.Val = Call;
      HighStmt Return;
      Return.Kind = StmtKind::Return;
      Return.RetVal =
          HighExpr::makeBinop(NdOp::CONCAT, HighExpr::makeUndef(4), Value());
      Return.RetVal->Type = NdType::makeInt(8, false);
      F.High.SourceTypeHint = Entry;
      F.High.ReturnType = Entry.ReturnType;
      F.High.Body = {Define, Return};
      switch (Mutation) {
      case 1:
        Call->SourceCallHint.reset();
        break;
      case 2:
        Hint->Signature.ReturnLocation.ValueBytes = 8;
        break;
      case 3:
        Hint->Signature.ReturnType = NdType::makeFloat(4);
        ASSERT_TRUE(
            assignDarwinScalarSourceABI(Hint->Signature, Architecture, Error));
        break;
      case 4:
        Hint->Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
        ASSERT_TRUE(
            assignDarwinScalarSourceABI(Hint->Signature, Architecture, Error));
        break;
      case 5:
        Hint->Signature.ReturnType = NdType::makeInt(2);
        ASSERT_TRUE(
            assignDarwinScalarSourceABI(Hint->Signature, Architecture, Error));
        Call->Type = sourceABICallResultType(Hint->Signature);
        break;
      case 6:
        Call->Type = NdType::makeInt(8, false);
        break;
      case 7:
        Hint->DoesNotReturn = true;
        break;
      case 8:
        Call->Operands.clear();
        break;
      case 9:
        Return.RetVal->Operands[0] = HighExpr::makeConst(0, 4);
        break;
      case 10:
        Define.Val = HighExpr::makeUndef(4);
        F.High.Body[0] = Define;
        break;
      case 11:
        F.High.Body.erase(F.High.Body.begin());
        break;
      case 12:
        Call->Operands.push_back(HighExpr::makeConst(0, 4));
        break;
      case 13:
        Call->MemoryOrdering = NdMemoryOrdering::Acquire;
        break;
      case 14:
        Call->Type = NdType::makeFloat(4);
        break;
      case 15:
        Hint->Signature.ReturnComponents = {Hint->Signature.ReturnLocation};
        break;
      case 16:
        Hint->Signature.ReturnLocation.Kind =
            SourceABICarrierKind::FloatingRegister;
        break;
      default:
        break;
      }
      const auto Refined = refineNativeSourceTypeHint(F.High, F.Audit);
      EXPECT_EQ(bool(Refined), Mutation == 0);
      if (Refined) {
        EXPECT_EQ(Refined->ReturnLocation.ValueBytes, 4U);
        EXPECT_TRUE(Hint->Signature.ReturnType->IsSigned);
        EXPECT_EQ(Return.RetVal->Operands[0]->Kind, ExprKind::Undef);
      }
    }
}

TEST(NativeSourceHints, IntegerPrefixCallersCannotObserveUnknownUpperWord) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (uint16_t Width : {4, 8}) {
      NativeFixture F(Architecture);
      const auto &TRI = getTargetRegInfo(Architecture);
      SourceFunctionTypeHint Callee, Entry;
      Callee.Origin = Entry.Origin =
          SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      Callee.ReturnType = NdType::makeInt(4, false);
      Entry.ReturnType = NdType::makeInt(Width, false);
      std::string Error;
      ASSERT_TRUE(assignDarwinScalarSourceABI(Callee, Architecture, Error));
      ASSERT_TRUE(assignDarwinScalarSourceABI(Entry, Architecture, Error));
      std::map<va_t, SourceFunctionTypeHint> Hints{{0x1000, Entry},
                                                   {0x1080, Callee}};
      LowFunc Low;
      Low.Entry = 0x1000;
      LowBlock B;
      B.Id = 0;
      B.StartAddr = 0x1000;
      B.EndAddr = 0x1008;
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.Addr = 0x1000;
      Call.Output = NdVar::reg(TRI.IntReturnReg, 8);
      Call.addInput(NdVar::cst(0x1080, 8));
      LowOp Return;
      Return.Opcode = NdOp::RETURN;
      Return.Addr = 0x1004;
      Return.addInput(NdVar::reg(TRI.IntReturnReg, Width));
      B.Ops = {Call, Return};
      Low.Blocks = {B};
      LowToMedConverter Converter;
      Converter.setSourceCallHintsEnabled(true);
      Converter.setSourceCalleeTypeHints(&Hints);
      Converter.setSourceEntryTypeHints(&Hints);
      F.Med = Converter.convert(Low, Architecture, BinaryFormat::MachO);
      recoverCallAbi(F.Med, Architecture, {{0x1080, "native_prefix"}});
      F.Med.SourceTypeHint = Entry;
      inferMedTypes(F.Med, Architecture);
      F.High = MedToHighConverter().convert(F.Med, Architecture);
      // Isolate return publication from the separately authenticated callee
      // closure. Even an allowed call cannot define its unreturned high word.
      const auto Limitation = sdk::sourceBodyLimitation(
          F.High, Entry, &F.Audit, [](const HighExpr &) { return true; });
      EXPECT_EQ(Limitation.empty(), Width == 4) << Limitation;
      if (Width == 8)
        EXPECT_NE(Limitation.find("unresolved"), std::string::npos)
            << Limitation;
    }
}

TEST(NativeSourceHints, IntegerPairPathsMeetBothWordsAcrossJoinsAndBackedges) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Loop : {false, true})
      for (bool MissingWord : {false, true}) {
        auto F = nativePairFixture(Architecture);
        const auto Primary = F.Med.Blocks[0].Ops[0];
        const auto Secondary = F.Med.Blocks[0].Ops[1];
        const auto Return = F.Med.Blocks[0].Ops.back();
        F.Med.Blocks[0].Ops = {Primary};
        F.Med.Blocks[0].Succs = {1, 2};
        F.Med.Blocks.resize(4);
        for (int I = 1; I <= 3; ++I)
          F.Med.Blocks[I].Id = I;
        F.Med.Blocks[1].Preds =
            Loop ? std::vector<int>{0, 1} : std::vector<int>{0};
        F.Med.Blocks[1].Succs =
            Loop ? std::vector<int>{1, 3} : std::vector<int>{3};
        F.Med.Blocks[1].Ops = {Secondary};
        F.Med.Blocks[2].Preds = {0};
        F.Med.Blocks[2].Succs = {3};
        F.Med.Blocks[2].Ops =
            MissingWord ? std::vector<MedOp>{} : std::vector<MedOp>{Secondary};
        F.Med.Blocks[3].Preds = {1, 2};
        F.Med.Blocks[3].Ops = {Return};
        std::string Error;
        const auto Hint = inferNativeSourceTypeHint(
            F.Image, F.Med, F.High, F.Audit, Error, nullptr, true);
        ASSERT_TRUE(Hint) << Error;
        EXPECT_EQ(Hint->ReturnType->Kind == NdTypeKind::Struct, !MissingWord);
      }
}

TEST(NativeSourceHints, NonReturningContractsRequireTerminalFlowAndBoundCalls) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 10; ++Mutation) {
      SCOPED_TRACE(Mutation);
      NativeFixture F(Architecture);
      F.Med.DoesNotReturn = F.High.DoesNotReturn = true;
      auto Binding = std::make_shared<SourceCallTypeHint>();
      Binding->CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
      Binding->TargetAddress = 0x3000;
      Binding->TargetName = "abort";
      Binding->DoesNotReturn = true;
      Binding->Signature.ReturnType = NdType::makeVoid();
      std::string Error;
      ASSERT_TRUE(
          assignDarwinScalarSourceABI(Binding->Signature, Architecture, Error));
      MedOp Call;
      Call.Opcode = NdOp::CALL;
      Call.DoesNotReturn = true;
      Call.SourceCallHint = Binding;
      Call.addInput(MedVar::makeConst(0x3000, 8));
      F.Med.Blocks[0].Ops = {Call};
      auto Expression = HighExpr::makeCall("abort", 0x3000, {});
      Expression->SourceCallHint = Binding;
      Expression->Type = NdType::makeVoid();
      HighStmt Statement;
      Statement.Kind = StmtKind::Call;
      Statement.CallExpr = Expression;
      F.High.Body = {Statement};
      if (Mutation == 1)
        F.Med.DoesNotReturn = false;
      if (Mutation == 2)
        F.High.DoesNotReturn = false;
      if (Mutation == 3) {
        F.Med.Blocks[0].Ops[0].DoesNotReturn = false;
        MedOp Return;
        Return.Opcode = NdOp::RETURN;
        F.Med.Blocks[0].Ops.push_back(Return);
      }
      if (Mutation == 4)
        F.Med.Blocks[0].Ops.clear();
      if (Mutation == 5)
        F.Med.Blocks[0].Ops[0].Opcode = NdOp::INTRINSIC;
      if (Mutation == 6)
        F.Med.Blocks[0].Ops[0].SourceCallHint.reset();
      if (Mutation == 7)
        Binding->DoesNotReturn = false;
      if (Mutation == 8)
        F.Med.Blocks[0].ExceptionalSuccs.emplace_back();
      if (Mutation == 9)
        F.Med.Blocks[0].Ops[0].addInput(MedVar::makeConst(0, 8));
      auto Hint = F.infer(Error);
      EXPECT_EQ(bool(Hint), Mutation == 0) << Error;
      if (Hint) {
        EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
        EXPECT_EQ(Hint->ReturnLocation.Kind, SourceABICarrierKind::None);
        EXPECT_EQ(Hint->ReturnLocation.ValueBytes, 0U);
      }
    }
}

TEST(NativeSourceHints, KeepsObservedIntegerLocationsWithoutUsingNames) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeFixture Fixture(Architecture);
    Fixture.Med.Name = "unrelated_stripped_symbol";
    std::string Error;
    auto Hint = Fixture.infer(Error);
    ASSERT_TRUE(Hint) << Error;
    EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::NativeAnalysis);
    ASSERT_EQ(Hint->Parameters.size(), 2U);
    const auto &TRI = getTargetRegInfo(Architecture);
    EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, TRI.IntParamRegs[0]);
    EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, TRI.IntParamRegs[1]);
    EXPECT_EQ(Hint->ReturnLocation.RegisterOffset, TRI.IntReturnReg);
    EXPECT_EQ(Hint->ReturnLocation.ValueBytes, 4U);
    EXPECT_EQ(Fixture.Med.ReturnValueEvidence, MedReturnValueEvidence::Unknown);
  }
}

TEST(NativeSourceHints, LeafReturnsPreserveProvenFullMachineWidth) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeFixture Fixture(Architecture);
    auto &Write = Fixture.Med.Blocks[0].Ops.front();
    Write.Opcode = NdOp::INT_ZEXT;
    Write.NumInputs = 1;
    Write.Output.Size = 8;
    Write.Inputs[0] = MedVar::makeConst(UINT32_MAX, 4);
    Fixture.High.Body.front().RetVal =
        HighExpr::makeUnary(NdOp::INT_ZEXT, HighExpr::makeConst(UINT32_MAX, 4));
    Fixture.High.Body.front().RetVal->Type = NdType::makeInt(8, false);
    std::string Error;
    const auto Full = Fixture.infer(Error);
    ASSERT_TRUE(Full) << Error;
    EXPECT_EQ(Full->ReturnType->Size, 8U);
    EXPECT_FALSE(Full->ReturnType->IsSigned);
    EXPECT_EQ(Full->ReturnLocation.ValueBytes, 8U);
    // A source expression alone cannot authorize a wider machine carrier.
    Write.Output.Size = 4;
    const auto Narrow = Fixture.infer(Error);
    ASSERT_TRUE(Narrow) << Error;
    EXPECT_EQ(Narrow->ReturnLocation.ValueBytes, 4U);
    Write.Output.Size = 8;
    Fixture.High.Body.front().RetVal = HighExpr::makeUndef(8);
    const auto Unknown = Fixture.infer(Error);
    ASSERT_TRUE(Unknown) << Error;
    EXPECT_EQ(Unknown->ReturnLocation.ValueBytes, 4U);
  }
}

TEST(NativeSourceHints, NarrowExternalResultsDoNotProveTheirUpperBits) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeFixture Fixture(Architecture);
    auto &Write = Fixture.Med.Blocks[0].Ops.front();
    Write.Opcode = NdOp::CALL;
    Write.NumInputs = 1;
    Write.Inputs[0] = MedVar::makeConst(0x1020, 8);
    Write.Output.Size = 8;
    auto Call = std::make_shared<SourceCallTypeHint>();
    Call->Signature.ReturnType = NdType::makeInt(4, false);
    std::string Error;
    ASSERT_TRUE(
        assignDarwinScalarSourceABI(Call->Signature, Architecture, Error));
    Write.SourceCallHint = Call;
    Fixture.High.Body.front().RetVal = HighExpr::makeConst(UINT32_MAX, 8);
    const auto Hint = Fixture.infer(Error);
    ASSERT_TRUE(Hint) << Error;
    EXPECT_EQ(Hint->ReturnLocation.ValueBytes, 4U);
  }
}

NativeFixture compilerRTPlatformVersionFixture(Arch Architecture) {
  NativeFixture Fixture(Architecture);
  const auto &TRI = getTargetRegInfo(Architecture);
  Fixture.Med.ReturnType = Fixture.High.ReturnType = NdType::makeInt(8);
  Fixture.Med.Blocks[0].Ops[0].Output.Size = 8;
  Fixture.High.Body[0].RetVal = HighExpr::makeConst(1, 8);
  for (unsigned Index = 2; Index < 4; ++Index) {
    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.Id = Index;
    Parameter.RegOff = TRI.IntParamRegs[Index];
    Parameter.Size = 4;
    Parameter.TheArch = Architecture;
    Fixture.Med.Params.push_back(Parameter);
    Fixture.Med.TypedParams.push_back(
        {"arg" + std::to_string(Index), NdType::makeInt(4)});
    Fixture.High.Params.push_back(
        {"arg" + std::to_string(Index), NdType::makeInt(4)});
  }
  SourceCallTypeHint Availability;
  Availability.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
  Availability.WeakImport = true;
  Availability.TargetAddress = 0x1080;
  Availability.TargetName = "_availability_version_check";
  Availability.Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
  Availability.Signature.ReturnType = NdType::makeInt(1, false);
  Availability.Signature.Parameters = {
      {"count", NdType::makeInt(4, false)},
      {"versions", NdType::makePtr(NdType::makeVoid())},
  };
  std::string Error;
  EXPECT_TRUE(
      assignDarwinFixedSourceABI(Availability.Signature, Architecture, Error))
      << Error;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.SourceCallHint =
      std::make_shared<const SourceCallTypeHint>(std::move(Availability));
  Call.addInput(MedVar::makeConst(0x1080, 8));
  Call.addInput(MedVar::makeConst(1, 4));
  Call.addInput(MedVar::makeConst(0x10a0, 8));
  Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin(), Call);
  Fixture.Audit.DecodedInstructions = Fixture.Audit.LiftedInstructions = 3;
  Fixture.Image.Symbols.push_back(
      {"___isPlatformVersionAtLeast", Fixture.Med.Entry, 0x80, true});
  return Fixture;
}

TEST(NativeSourceHints,
     CompilerRTPlatformVersionHelperUsesExactNarrowContract) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Fixture = compilerRTPlatformVersionFixture(Architecture);
    std::string Error;
    const auto Hint = Fixture.infer(Error);
    ASSERT_TRUE(Hint) << Error;
    ASSERT_TRUE(Hint->ReturnType);
    EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Int);
    EXPECT_EQ(Hint->ReturnType->Size, 4U);
    EXPECT_TRUE(Hint->ReturnType->IsSigned);
    ASSERT_EQ(Hint->Parameters.size(), 4U);
    const auto &TRI = getTargetRegInfo(Architecture);
    for (size_t I = 0; I < Hint->Parameters.size(); ++I) {
      EXPECT_EQ(Hint->Parameters[I].Type->Kind, NdTypeKind::Int);
      EXPECT_EQ(Hint->Parameters[I].Type->Size, 4U);
      EXPECT_FALSE(Hint->Parameters[I].Type->IsSigned);
      EXPECT_EQ(Hint->Parameters[I].Location.RegisterOffset,
                TRI.IntParamRegs[I]);
      EXPECT_EQ(Hint->Parameters[I].Location.ValueBytes, 4U);
    }
  }
}

TEST(NativeSourceHints,
     CompilerRTPlatformVersionHelperRejectsContradictoryEvidence) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      auto Fixture = compilerRTPlatformVersionFixture(Architecture);
      auto &Symbol = Fixture.Image.Symbols.back();
      auto Call = std::make_shared<SourceCallTypeHint>(
          *Fixture.Med.Blocks[0].Ops[0].SourceCallHint);
      Fixture.Med.Blocks[0].Ops[0].SourceCallHint = Call;
      if (Mutation == 0)
        Symbol.IsFunc = false;
      if (Mutation == 1)
        Fixture.Image.Symbols.push_back(Symbol);
      if (Mutation == 2)
        Call->WeakImport = false;
      if (Mutation == 3)
        Call->TargetName = "_different";
      if (Mutation == 4)
        Call->CallKind = SourceCallTypeHint::Kind::Native;
      if (Mutation == 5)
        Call->Signature.ReturnType = NdType::makeInt(4, false);
      std::string Error;
      EXPECT_FALSE(Fixture.infer(Error)) << Mutation;
      EXPECT_FALSE(Error.empty()) << Mutation;
    }

    for (unsigned Mutation = 0; Mutation < 2; ++Mutation) {
      auto Unrelated = compilerRTPlatformVersionFixture(Architecture);
      if (Mutation == 0)
        Unrelated.Image.Symbols.back().Name = "unrelated_local_helper";
      else
        Unrelated.Image.Symbols.back().Addr += 4;
      std::string Error;
      const auto Generic = Unrelated.infer(Error);
      ASSERT_TRUE(Generic) << Mutation << ": " << Error;
      EXPECT_EQ(Generic->ReturnType->Size, 8U) << Mutation;
    }
  }
}

struct NativeVoidFixture : NativeFixture {
  LowFunc Low;
  NativeVoidFixture(Arch Architecture, bool Indirect = false)
      : NativeFixture(Architecture) {
    Med.Params[0].Size = 8;
    Med.TypedParams[0].Type = High.Params[0].Type = NdType::makeInt(8);
    Med.Params[1].Id = -1;
    auto Hint = std::make_shared<SourceCallTypeHint>();
    Hint->CallKind = SourceCallTypeHint::Kind::SwiftRuntimeCall;
    Hint->Signature.ReturnType = NdType::makeVoid();
    Hint->Signature.Parameters.push_back(
        {"object", NdType::makePtr(NdType::makeVoid())});
    std::string Error;
    EXPECT_TRUE(
        assignDarwinScalarSourceABI(Hint->Signature, Architecture, Error));
    MedOp Call;
    Call.Opcode = Indirect ? NdOp::INDIR_CALL : NdOp::CALL;
    Call.Addr = 0x1010;
    Call.OriginSeq = 7;
    Call.SourceCallHint = Hint;
    Call.addInput(MedVar::makeConst(0x1080, 8));
    Call.addInput(Med.Params[0]);
    auto Return = Med.Blocks[0].Ops.back();
    Return.Addr = Call.Addr;
    Med.Blocks[0].Ops = {Call, Return};
    Low.Entry = Med.Entry;
    Low.Blocks.emplace_back();
    Low.Blocks[0].Id = 0;
    LowOp NativeCall;
    NativeCall.Opcode = Call.Opcode;
    NativeCall.Addr = Call.Addr;
    NativeCall.Seq = Call.OriginSeq;
    NativeCall.Output =
        NdVar::reg(getTargetRegInfo(Architecture).IntReturnReg, 8);
    NativeCall.addInput(NdVar::cst(0x1080, 8));
    LowOp NativeReturn;
    NativeReturn.Opcode = NdOp::RETURN;
    NativeReturn.Addr = Call.Addr;
    Low.Blocks[0].Ops = {NativeCall, NativeReturn};
  }
  std::optional<SourceFunctionTypeHint> inferVoid(std::string &Error) const {
    return inferNativeSourceTypeHint(Image, Med, High, Audit, Error, &Low);
  }

  void useValueWitnessDestroy() {
    const auto &TRI = getTargetRegInfo(Image.Arch);
    auto Hint = swiftValueWitnessSourceCallHint(
        Image.Arch, SourceCallTypeHint::SwiftValueWitnessKind::Destroy);
    ASSERT_TRUE(Hint);
    auto &Call = Med.Blocks[0].Ops[0];
    ASSERT_TRUE(Call.Opcode == NdOp::CALL || Call.Opcode == NdOp::INDIR_CALL);
    MedVar Target;
    Target.Kind = MedVar::Reg;
    Target.Id = 20;
    Target.SSAVer = 1;
    Target.Size = 8;
    Target.TheArch = Image.Arch;
    Target.RegOff = TRI.IntReturnReg;
    Med.Params[1].Id = 1;
    Med.Params[1].Size = 8;
    Med.TypedParams[0].Type = High.Params[0].Type =
        NdType::makePtr(NdType::makeVoid());
    Med.TypedParams[1].Type = High.Params[1].Type =
        NdType::makePtr(NdType::makeVoid());
    Call.Opcode = NdOp::INDIR_CALL;
    Call.SourceCallHint =
        std::make_shared<const SourceCallTypeHint>(std::move(*Hint));
    Call.NumInputs = 0;
    Call.addInput(Target);
    Call.addInput(Med.Params[0]);
    Call.addInput(Med.Params[1]);

    LowOp *NativeCall = nullptr;
    for (auto &Block : Low.Blocks)
      for (auto &Operation : Block.Ops)
        if (Operation.Opcode == NdOp::CALL ||
            Operation.Opcode == NdOp::INDIR_CALL) {
          ASSERT_EQ(NativeCall, nullptr);
          NativeCall = &Operation;
        }
    ASSERT_NE(NativeCall, nullptr);
    NativeCall->Opcode = NdOp::INDIR_CALL;
    NativeCall->NumInputs = 0;
    NativeCall->addInput(NdVar::reg(TRI.IntReturnReg, 8));
  }

  void useValueWitnessStoreTag() {
    useValueWitnessDestroy();
    auto Hint = swiftValueWitnessSourceCallHint(
        Image.Arch,
        SourceCallTypeHint::SwiftValueWitnessKind::StoreEnumTagSinglePayload);
    ASSERT_TRUE(Hint);
    auto &Call = Med.Blocks[0].Ops[0];
    Call.SourceCallHint = std::make_shared<const SourceCallTypeHint>(*Hint);
    const auto Target = Call.Inputs[0];
    Call.NumInputs = 0;
    Call.addInput(Target);
    Med.Params.clear();
    Med.TypedParams.clear();
    High.Params.clear();
    for (const auto &P : Hint->Signature.Parameters) {
      MedVar Parameter;
      Parameter.Kind = MedVar::Param;
      Parameter.Id = Med.Params.size();
      Parameter.Size = P.Type->Size;
      Parameter.RegOff = P.Location.RegisterOffset;
      Parameter.TheArch = Image.Arch;
      Med.Params.push_back(Parameter);
      Med.TypedParams.push_back({P.Name, P.Type});
      High.Params.push_back({P.Name, P.Type});
      Call.addInput(Parameter);
    }
  }

  void useNativeVoidCallee() {
    auto Hint = std::make_shared<SourceCallTypeHint>(
        *Med.Blocks[0].Ops[0].SourceCallHint);
    Hint->CallKind = SourceCallTypeHint::Kind::Native;
    Hint->TargetAddress = 0x1080;
    Hint->TargetName = "native_void_callee";
    Hint->Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Med.Blocks[0].Ops[0].SourceCallHint = std::move(Hint);
  }
};

TEST(NativeSourceHints, VoidBlockInvocationNeedsCurrentReceiverProof) {
  NativeVoidFixture Fixture(Arch::AArch64, true);
  auto &Call = Fixture.Med.Blocks[0].Ops[0];
  auto Binding = std::make_shared<SourceCallTypeHint>();
  Binding->CallKind = SourceCallTypeHint::Kind::BlockInvoke;
  Binding->Signature.Origin =
      SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Binding->Signature.ReturnType = NdType::makeVoid();
  Binding->Signature.Parameters = {
      {"block", NdType::makePtr(NdType::makeVoid())}};
  std::string Error;
  ASSERT_TRUE(
      assignDarwinScalarSourceABI(Binding->Signature, Arch::AArch64, Error))
      << Error;
  Call.SourceCallHint = Binding;
  MedVar Target;
  Target.Kind = MedVar::Reg;
  Target.Id = 20;
  Target.SSAVer = 1;
  Target.Size = 8;
  Target.TheArch = Arch::AArch64;
  Target.RegOff = a64reg::X8;
  Call.Inputs[0] = Target;
  EXPECT_FALSE(Fixture.inferVoid(Error));

  // Even a matching-looking dynamic branch remains unbound without a
  // current block+16 target load and the same receiver on the call edge.
  Fixture.Low.Blocks[0].Ops[0].Inputs[0] = NdVar::reg(a64reg::X8, 8);
  EXPECT_FALSE(Fixture.inferVoid(Error));
}

TEST(NativeSourceHints, VoidTailForwardersNeverSupplyAnUnprovenResult) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Indirect : {false, true}) {
      NativeVoidFixture Fixture(Architecture, Indirect);
      std::string Error;
      const auto Hint = Fixture.inferVoid(Error);
      ASSERT_TRUE(Hint) << Error;
      EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
      EXPECT_EQ(Hint->ReturnLocation.Kind, SourceABICarrierKind::None);
      EXPECT_EQ(Hint->ReturnLocation.ValueBytes, 0U);
      ASSERT_EQ(Hint->Parameters.size(), 1U);
      EXPECT_EQ(Hint->Parameters[0].Type->Kind, NdTypeKind::Ptr);
      EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset,
                getTargetRegInfo(Architecture).IntParamRegs[0]);
      EXPECT_FALSE(Fixture.infer(Error));
      EXPECT_EQ(Fixture.Med.ReturnType->Kind, NdTypeKind::Int);
      EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                MedReturnValueEvidence::Unknown);

      // Keep the established leaf subset available when the general frame
      // proof rejects an otherwise harmless LowIR operation. The narrower
      // proof must still carry stack-address taint through that operation to
      // a declared call argument.
      NativeVoidFixture Compatibility(Architecture, Indirect);
      LowOp Intrinsic;
      Intrinsic.Opcode = NdOp::INTRINSIC;
      Intrinsic.Addr = 0x1004;
      Intrinsic.Output =
          NdVar::reg(getTargetRegInfo(Architecture).IntParamRegs[1], 8);
      Compatibility.Low.Blocks[0].Ops.insert(
          Compatibility.Low.Blocks[0].Ops.begin(), Intrinsic);
      EXPECT_TRUE(Compatibility.inferVoid(Error)) << Error;
      Compatibility.Low.Blocks[0].Ops[0].Output =
          NdVar::reg(getTargetRegInfo(Architecture).IntParamRegs[0], 8);
      Compatibility.Low.Blocks[0].Ops[0].addInput(
          NdVar::reg(getTargetRegInfo(Architecture).StackPointer, 8));
      EXPECT_FALSE(Compatibility.inferVoid(Error));
    }
}

TEST(NativeSourceHints, VoidContractsPropagateAcrossExactNativeCallees) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeVoidFixture Fixture(Architecture);
    Fixture.useNativeVoidCallee();
    std::string Error;
    const auto Hint = Fixture.inferVoid(Error);
    ASSERT_TRUE(Hint) << Error;
    EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);

    auto Forged = std::make_shared<SourceCallTypeHint>(
        *Fixture.Med.Blocks[0].Ops[0].SourceCallHint);
    Forged->TargetAddress += 4;
    Fixture.Med.Blocks[0].Ops[0].SourceCallHint = std::move(Forged);
    EXPECT_FALSE(Fixture.inferVoid(Error));
  }
}

TEST(NativeSourceHints, ImplicitCallInputsRequireCompleteEntryByteEvidence) {
  for (const auto [Architecture, Volatile] :
       {std::pair{Arch::AArch64, false}, std::pair{Arch::AArch64, true},
        std::pair{Arch::X64, false}, std::pair{Arch::X64, true}})
    for (unsigned Mutation = 0; Mutation < 10; ++Mutation) {
      SCOPED_TRACE(static_cast<unsigned>(Architecture));
      SCOPED_TRACE(Volatile);
      SCOPED_TRACE(Mutation);
      NativeVoidFixture F(Architecture);
      F.useNativeVoidCallee();
      const auto Context = Architecture == Arch::AArch64
                               ? (Volatile ? a64reg::X8 : a64reg::X20)
                               : (Volatile ? x86reg::R10 : x86reg::RBX);
      auto &Call = F.Med.Blocks[0].Ops[0];
      auto Hint = std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
      Hint->Signature.Parameters.push_back(
          {"context",
           NdType::makeInt(8),
           {SourceABICarrierKind::IntegerRegister, Context, 0, 8}});
      Call.SourceCallHint = Hint;
      MedVar Input;
      Input.Kind = MedVar::Reg;
      Input.RegOff = Context;
      Input.Id = 200;
      Input.Size = 8;
      Input.TheArch = Architecture;
      Call.addInput(Input);
      auto &Ops = F.Low.Blocks[0].Ops;
      if (Mutation == 1)
        ++Ops[0].Seq;
      if (Mutation == 2)
        Ops[0].Inputs[0] = NdVar::cst(0x1084, 8);
      if (Mutation == 3)
        Hint->TargetAddress += 4;
      if (Mutation == 4)
        Call.Inputs[2].SSAVer = 1;
      if (Mutation == 5) {
        Hint->Signature.Parameters.back().Type = NdType::makeInt(4);
        Hint->Signature.Parameters.back().Location.ValueBytes = 4;
        Call.Inputs[2].Size = 4;
      }
      if (Mutation == 6 || Mutation == 7) {
        LowOp Write;
        Write.Opcode = NdOp::COPY;
        Write.Addr = 0x1000;
        Write.Output = NdVar::reg(Context, Mutation == 6 ? 8 : 4);
        Write.addInput(NdVar::cst(0, Write.Output.Size));
        Ops.insert(Ops.begin(), Write);
      }
      if (Mutation == 8)
        F.Low.Blocks[0].Succs.push_back(99);
      if (Mutation == 9)
        Hint->Signature.Architecture = Arch::Unknown;
      std::string Error;
      const auto Result = F.inferVoid(Error);
      if (Mutation == 0) {
        ASSERT_TRUE(Result) << Error;
        ASSERT_EQ(Result->Parameters.size(), 2U);
        EXPECT_EQ(Result->Parameters[1].Location.RegisterOffset, Context);
        EXPECT_EQ(Result->Parameters[1].Location.ValueBytes, 8U);
      } else if (Result) {
        EXPECT_TRUE(std::none_of(Result->Parameters.begin(),
                                 Result->Parameters.end(), [&](const auto &P) {
                                   return P.Location.RegisterOffset == Context;
                                 }));
      }
    }
}

TEST(NativeSourceHints, TailContextSurvivesNativeInferenceAndRelifting) {
  for (const auto [Architecture, Volatile] :
       {std::pair{Arch::AArch64, false}, std::pair{Arch::AArch64, true},
        std::pair{Arch::X64, false}, std::pair{Arch::X64, true}}) {
    SCOPED_TRACE(static_cast<unsigned>(Architecture));
    SCOPED_TRACE(Volatile);
    NativeFixture F(Architecture);
    const auto ContextRegister = Architecture == Arch::AArch64
                                     ? (Volatile ? a64reg::X8 : a64reg::X20)
                                     : (Volatile ? x86reg::R10 : x86reg::RBX);
    auto &Bytes = F.Image.Segments[0].Data;
    if (Architecture == Arch::AArch64) {
      // b helper; helper: add x0, x0, x20; ret
      llvm::support::endian::write32le(Bytes.data(), 0x14000010);
      llvm::support::endian::write32le(Bytes.data() + 0x40,
                                       Volatile ? 0x8b080000 : 0x8b140000);
      llvm::support::endian::write32le(Bytes.data() + 0x44, 0xd65f03c0);
    } else {
      // jmp helper; helper: lea rax, [rdi + rbx]; ret
      const uint8_t Jump[] = {0xe9, 0x3b, 0, 0, 0};
      const uint8_t Body[] = {uint8_t(Volatile ? 0x4a : 0x48), 0x8d, 0x04,
                              uint8_t(Volatile ? 0x17 : 0x1f), 0xc3};
      std::copy(std::begin(Jump), std::end(Jump), Bytes.begin());
      std::copy(std::begin(Body), std::end(Body), Bytes.begin() + 0x40);
    }
    F.Image.Symbols = {{"forward_context", 0x1000,
                        Architecture == Arch::AArch64 ? 4U : 5U, true},
                       {"read_context", 0x1040,
                        Architecture == Arch::AArch64 ? 8U : 5U, true}};
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {0x1000, 0x1040};
    llvm::LLVMContext Context;
    Pipeline Engine;
    auto Result = Engine.run(F.Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    auto Infer = [&](va_t Entry) {
      const LowFunc *Low = nullptr;
      const MedFunc *Med = nullptr;
      const HighFunc *High = nullptr;
      const PipelineFunctionAudit *Audit = nullptr;
      for (const auto &V : Result.LowFuncs)
        if (V.Entry == Entry)
          Low = &V;
      for (const auto &V : Result.MedFuncs)
        if (V.Entry == Entry)
          Med = &V;
      for (const auto &V : Result.HighFuncs)
        if (V.Entry == Entry)
          High = &V;
      for (const auto &V : Result.FunctionAudits)
        if (V.Entry == Entry)
          Audit = &V;
      EXPECT_TRUE(Low && Med && High && Audit);
      std::string Error;
      const auto Hint = Low && Med && High && Audit
                            ? inferNativeSourceTypeHint(F.Image, *Med, *High,
                                                        *Audit, Error, Low)
                            : std::nullopt;
      EXPECT_TRUE(Hint) << Error;
      return Hint;
    };
    const auto Helper = Infer(0x1040);
    ASSERT_TRUE(Helper);
    Options.SourceTypeHints.emplace(0x1040, *Helper);
    Result = Engine.run(F.Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    const auto Forward = Infer(0x1000);
    ASSERT_TRUE(Forward);
    ASSERT_EQ(Forward->Parameters.size(), 2U);
    EXPECT_TRUE(std::any_of(Forward->Parameters.begin(),
                            Forward->Parameters.end(), [&](const auto &P) {
                              return P.Location.RegisterOffset ==
                                         ContextRegister &&
                                     P.Location.ValueBytes == 8;
                            }));
    Options.SourceTypeHints.emplace(0x1000, *Forward);
    Result = Engine.run(F.Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    unsigned Verified = 0;
    for (const auto &High : Result.HighFuncs) {
      const auto Audit = std::find_if(
          Result.FunctionAudits.begin(), Result.FunctionAudits.end(),
          [&](const auto &A) { return A.Entry == High.Entry; });
      ASSERT_NE(Audit, Result.FunctionAudits.end());
      ASSERT_TRUE(High.SourceTypeHint);
      const auto Limitation =
          sdk::sourceBodyLimitation(High, *High.SourceTypeHint, &*Audit,
                                    [](const HighExpr &) { return true; });
      EXPECT_TRUE(Limitation.empty()) << Limitation;
      ++Verified;
    }
    EXPECT_EQ(Verified, 2U);
  }
}

TEST(NativeSourceHints, ForwardedSwiftResultAddressIsAnObservedEntryInput) {
  using namespace immutable_native_call_test;
  constexpr va_t ImportSlot = runtime_function_address_test::Slot;
  Fixture F;
  const std::string Name = "_$ss11AnyHashableVyABxcSHRzlufC";
  F.Image.ImportPtrSlots[ImportSlot] = Name;
  F.Image.DyldBindSlots[ImportSlot] = {
      Name, 0, "/usr/lib/swift/libswiftCore.dylib", false};
  F.Image.Symbols[0] = {"forward_result", Entry, 20, true};
  // Only a normal prologue, the original BL, and the epilogue. The incoming
  // x8 result address is observed by the complete Swift call ABI, never by
  // an explicit LowIR input or an assumed entry signature.
  const uint32_t Words[] = {0xa9bf7bfd, 0x910003fd, 0x9400003e, 0xa8c17bfd,
                            0xd65f03c0};
  for (unsigned I = 0; I < std::size(Words); ++I)
    F.word(I, Words[I]);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries = {Entry};
  F.Result = Pipeline().run(F.Image, F.Context, Options);
  ASSERT_TRUE(F.Result.Success) << F.Result.Error;
  ASSERT_TRUE(F.low() && F.med() && F.high());
  const auto Audit = std::find_if(
      F.Result.FunctionAudits.begin(), F.Result.FunctionAudits.end(),
      [&](const auto &A) { return A.Entry == Entry; });
  ASSERT_NE(Audit, F.Result.FunctionAudits.end());
  std::string Error;
  const auto Hint = inferNativeSourceTypeHint(F.Image, *F.med(), *F.high(),
                                              *Audit, Error, F.low());
  ASSERT_TRUE(Hint) << Error;
  ASSERT_EQ(Hint->Parameters.size(), 4U);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
  EXPECT_EQ(Hint->Parameters[3].Location.RegisterOffset, a64reg::X8);
  EXPECT_EQ(Hint->Parameters[3].Location.ValueBytes, 8U);
  // This is an internal source summary, not a guessed external Swift entry.
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::NativeAnalysis);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::C);
  Options.SourceTypeHints.emplace(Entry, *Hint);
  F.Result = Pipeline().run(F.Image, F.Context, Options);
  ASSERT_TRUE(F.Result.Success) << F.Result.Error;
  ASSERT_TRUE(F.high() && F.high()->SourceTypeHint);
  ASSERT_EQ(F.high()->Params.size(), 4U);
  const auto CurrentAudit = std::find_if(
      F.Result.FunctionAudits.begin(), F.Result.FunctionAudits.end(),
      [&](const auto &A) { return A.Entry == Entry; });
  ASSERT_NE(CurrentAudit, F.Result.FunctionAudits.end());
  const auto Allowed = [&](const HighExpr &E) {
    return sdk::objcSourceCallBound(E, F.Image, {}, nullptr, nullptr, F.high());
  };
  EXPECT_TRUE(sdk::sourceBodyLimitation(*F.high(), *F.high()->SourceTypeHint,
                                        &*CurrentAudit, Allowed)
                  .empty());
  unsigned Calls = 0;
  walkStmts(F.high()->Body, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &E) {
      if (!E || E->Kind != ExprKind::Call)
        return;
      ++Calls;
      ASSERT_TRUE(E->SourceCallHint);
      ASSERT_EQ(E->Operands.size(), 4U);
      const auto &Result = E->SourceCallHint->Signature.Parameters[0];
      EXPECT_EQ(Result.TheRole,
                SourceParameterTypeHint::Role::SwiftIndirectResult);
      EXPECT_EQ(Result.Location.RegisterOffset, a64reg::X8);
      EXPECT_TRUE(Allowed(*E));
    });
  });
  EXPECT_EQ(Calls, 1U);
}

TEST(NativeSourceHints, VoidContractsAcceptExactSwiftStringBridgeCallBindings) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (auto Kind : {SourceCallTypeHint::Kind::SwiftStringBridge,
                      SourceCallTypeHint::Kind::SwiftStringFromNSString}) {
      NativeVoidFixture Fixture(Architecture);
      auto Hint = std::make_shared<SourceCallTypeHint>(
          *Fixture.Med.Blocks[0].Ops[0].SourceCallHint);
      Hint->CallKind = Kind;
      Hint->Signature.Origin =
          SourceFunctionTypeHint::OriginKind::SwiftStringBridge;
      Fixture.Med.Blocks[0].Ops[0].SourceCallHint = std::move(Hint);
      std::string Error;
      const auto Result = Fixture.inferVoid(Error);
      ASSERT_TRUE(Result) << Error;
      EXPECT_EQ(Result->ReturnType->Kind, NdTypeKind::Void);
    }
}

TEST(NativeSourceHints,
     VoidTailContractsRejectHiddenOutputsAndIncompleteEvidence) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 15; ++Mutation) {
      NativeVoidFixture Fixture(Architecture);
      const auto &TRI = getTargetRegInfo(Architecture);
      auto &Ops = Fixture.Low.Blocks[0].Ops;
      switch (Mutation) {
      case 0:
        ++Ops[1].Addr;
        break;
      case 1:
      case 2:
      case 3: {
        LowOp Write;
        Write.Opcode = NdOp::COPY;
        const auto Register = Mutation == 1                   ? TRI.StackPointer
                              : Mutation == 2                 ? TRI.FramePointer
                              : Architecture == Arch::AArch64 ? a64reg::X19
                                                              : x86reg::RBX;
        Write.Output = NdVar::reg(Register, 8);
        Write.addInput(NdVar::cst(0, 8));
        Ops.insert(Ops.begin(), Write);
        break;
      }
      case 4:
        Ops[0].Inputs[0].Offset += 8;
        break;
      case 5:
        Ops[0].Addr += 4;
        Ops[1].Addr += 4;
        break;
      case 6:
      case 7:
      case 8: {
        auto Hint = std::make_shared<SourceCallTypeHint>(
            *Fixture.Med.Blocks[0].Ops[0].SourceCallHint);
        if (Mutation == 6) {
          // A changed return type without its matching ABI is not a valid
          // source contract, even when the helper supplies no result.
          Hint->Signature.ReturnType = NdType::makeInt(8);
        } else if (Mutation == 7) {
          Hint->CallKind = SourceCallTypeHint::Kind::Native;
        } else {
          Hint->DoesNotReturn = true;
        }
        Fixture.Med.Blocks[0].Ops[0].SourceCallHint = Hint;
        break;
      }
      case 9:
        Fixture.Low.Entry += 4;
        break;
      case 10:
        Fixture.Low.Blocks.clear();
        break;
      case 11:
        Fixture.Med.Blocks[0].Preds = {17};
        break;
      case 12:
        Ops[0].Inputs[0] = NdVar::reg(TRI.IntParamRegs[0], 8);
        break;
      case 13:
        Ops[0].NumInputs = static_cast<uint8_t>(std::size(Ops[0].Inputs) + 1);
        break;
      case 14: {
        LowOp Escape;
        Escape.Opcode = NdOp::COPY;
        Escape.Addr = Ops[0].Addr;
        Escape.Output = NdVar::reg(TRI.IntParamRegs[0], 8);
        Escape.addInput(NdVar::reg(TRI.StackPointer, 8));
        Ops.insert(Ops.begin(), Escape);
        break;
      }
      }
      std::string Error;
      EXPECT_FALSE(Fixture.inferVoid(Error)) << Mutation << ": " << Error;
    }
}

TEST(NativeSourceHints,
     CanonicalDynamicValueWitnessDestroyHasAnExactVoidCallIdentity) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeVoidFixture Leaf(Architecture);
    Leaf.useValueWitnessDestroy();
    std::string Error;
    const auto LeafHint = Leaf.inferVoid(Error);
    ASSERT_TRUE(LeafHint) << Error;
    EXPECT_EQ(LeafHint->ReturnType->Kind, NdTypeKind::Void);
  }
}

TEST(NativeSourceHints, CanonicalSinglePayloadStorePreservesVoidCallEffects) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeVoidFixture Fixture(Architecture);
    Fixture.useValueWitnessStoreTag();
    std::string Error;
    const auto Hint = Fixture.inferVoid(Error);
    ASSERT_TRUE(Hint) << Error;
    EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
    ASSERT_EQ(Hint->Parameters.size(), 4U);
    EXPECT_EQ(Hint->Parameters[1].Type->Size, 4U);
    EXPECT_EQ(Hint->Parameters[2].Type->Size, 4U);
  }
}

TEST(NativeSourceHints,
     DynamicVoidCallIdentityRejectsForgeryMismatchAndFrameTargets) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 9; ++Mutation) {
      NativeVoidFixture Fixture(Architecture);
      Fixture.useValueWitnessDestroy();
      auto &MedCall = Fixture.Med.Blocks[0].Ops[0];
      auto &LowCall = Fixture.Low.Blocks[0].Ops[0];
      const auto &TRI = getTargetRegInfo(Architecture);
      switch (Mutation) {
      case 0: {
        auto Hint =
            std::make_shared<SourceCallTypeHint>(*MedCall.SourceCallHint);
        Hint->TargetName += "_forged";
        MedCall.SourceCallHint = std::move(Hint);
        break;
      }
      case 1: {
        auto Hint = swiftValueWitnessSourceCallHint(
            Architecture,
            SourceCallTypeHint::SwiftValueWitnessKind::InitializeWithCopy);
        ASSERT_TRUE(Hint);
        MedCall.SourceCallHint =
            std::make_shared<const SourceCallTypeHint>(std::move(*Hint));
        break;
      }
      case 2:
        MedCall.Opcode = NdOp::CALL;
        break;
      case 3:
        LowCall.Opcode = NdOp::CALL;
        break;
      case 4:
        LowCall.Inputs[0] = NdVar::cst(0x1080, 8);
        break;
      case 5:
        MedCall.Inputs[0] = MedVar::makeConst(0x1080, 8);
        break;
      case 6: {
        LowOp Derive;
        Derive.Opcode = NdOp::COPY;
        Derive.Addr = 0x1008;
        Derive.Output = NdVar::reg(TRI.IntReturnReg, 8);
        Derive.addInput(NdVar::reg(TRI.StackPointer, 8));
        Fixture.Low.Blocks[0].Ops.insert(Fixture.Low.Blocks[0].Ops.begin(),
                                         std::move(Derive));
        break;
      }
      case 7:
        LowCall.Inputs[0].Size = 4;
        break;
      case 8:
        ++LowCall.Seq;
        break;
      }
      std::string Error;
      EXPECT_FALSE(Fixture.inferVoid(Error))
          << unsigned(Architecture) << ": " << Mutation << ": " << Error;
    }
}

struct NativeVoidFrameFixture : NativeVoidFixture {
  Arch Architecture;
  std::vector<uint64_t> Saved;
  size_t CallIndex = 0;
  size_t RestoreIndex = 0;
  int64_t FrameBytes;

  static LowOp op(NdOp Opcode, NdVar Output,
                  std::initializer_list<NdVar> Inputs, va_t Address = 0x1004) {
    LowOp Result;
    Result.Opcode = Opcode;
    Result.Output = Output;
    Result.Addr = Address;
    for (auto Input : Inputs)
      Result.addInput(Input);
    return Result;
  }

  explicit NativeVoidFrameFixture(Arch Architecture)
      : NativeVoidFixture(Architecture), Architecture(Architecture),
        FrameBytes(Architecture == Arch::AArch64 ? 32 : 24) {
    const auto &TRI = getTargetRegInfo(Architecture);
    Saved = {Architecture == Arch::AArch64 ? a64reg::X19 : x86reg::RBX,
             TRI.FramePointer};
    if (Architecture == Arch::AArch64) {
      Saved.push_back(TRI.LinkRegister);
      Saved.push_back(TRI.VecRegBase + 8 * TRI.VecRegStride);
    }
    const auto Call = Low.Blocks[0].Ops[0];
    auto &Ops = Low.Blocks[0].Ops;
    const auto SP = NdVar::reg(TRI.StackPointer, 8);
    const auto Address = NdVar::tmp(TmpBase, 8);
    Ops = {op(NdOp::INT_SUB, SP, {SP, NdVar::cst(FrameBytes, 8)})};
    for (size_t I = 0; I < Saved.size(); ++I) {
      Ops.push_back(op(NdOp::INT_ADD, Address, {SP, NdVar::cst(I * 8, 8)}));
      Ops.push_back(op(NdOp::STORE, {}, {Address, NdVar::reg(Saved[I], 8)}));
      Ops.push_back(
          op(NdOp::COPY, NdVar::reg(Saved[I], 8), {NdVar::cst(0, 8)}));
    }
    CallIndex = Ops.size();
    Ops.push_back(Call);
    RestoreIndex = Ops.size();
    for (size_t I = 0; I < Saved.size(); ++I) {
      Ops.push_back(
          op(NdOp::INT_ADD, Address, {SP, NdVar::cst(I * 8, 8)}, 0x1020));
      Ops.push_back(op(NdOp::LOAD, NdVar::reg(Saved[I], 8), {Address}, 0x1020));
    }
    Ops.push_back(
        op(NdOp::INT_ADD, SP, {SP, NdVar::cst(FrameBytes, 8)}, 0x1020));
    Ops.push_back(op(
        NdOp::RETURN, {},
        {NdVar::reg(TRI.LinkRegister ? TRI.LinkRegister : TRI.IntReturnReg, 8)},
        0x1024));
  }

  void splitReturns(bool Loop = false) {
    auto &First = Low.Blocks[0];
    const std::vector<LowOp> Restore(First.Ops.begin() + RestoreIndex,
                                     First.Ops.end());
    First.Ops.resize(RestoreIndex);
    First.Succs = Loop ? std::vector<int>{1} : std::vector<int>{1, 2};
    LowBlock Left, Right;
    Left.Id = 1;
    Right.Id = 2;
    Left.Preds = Loop ? std::vector<int>{0, 1} : std::vector<int>{0};
    Right.Preds = Loop ? std::vector<int>{1} : std::vector<int>{0};
    Left.Ops = Loop ? std::vector<LowOp>{op(NdOp::NOP, {}, {})} : Restore;
    Right.Ops = Restore;
    if (Loop)
      Left.Succs = {1, 2};
    Low.Blocks.push_back(std::move(Left));
    Low.Blocks.push_back(std::move(Right));
  }

  size_t usePreindexedSpills() {
    auto &Ops = Low.Blocks[0].Ops;
    const auto &TRI = getTargetRegInfo(Architecture);
    const auto SP = NdVar::reg(TRI.StackPointer, 8);
    const auto Base = NdVar::tmp(TmpBase + 64, 8);
    const auto Address = NdVar::tmp(TmpBase, 8);
    std::vector<LowOp> Prologue{
        op(NdOp::INT_SUB, Base, {SP, NdVar::cst(FrameBytes, 8)})};
    for (size_t I = 0; I < Saved.size(); ++I) {
      Prologue.push_back(
          op(NdOp::INT_ADD, Address, {Base, NdVar::cst(I * 8, 8)}));
      Prologue.push_back(
          op(NdOp::STORE, {}, {Address, NdVar::reg(Saved[I], 8)}));
      Prologue.push_back(
          op(NdOp::COPY, NdVar::reg(Saved[I], 8), {NdVar::cst(0, 8)}));
    }
    const size_t StackWrite = Prologue.size();
    Prologue.push_back(op(NdOp::COPY, SP, {Base}));
    Ops.erase(Ops.begin(), Ops.begin() + CallIndex);
    Ops.insert(Ops.begin(), Prologue.begin(), Prologue.end());
    CallIndex = Prologue.size();
    RestoreIndex++;
    return StackWrite;
  }
};

struct NativeFloatingFrameFixture : NativeVoidFrameFixture {
  static constexpr va_t ImportSlot = 0x3000;

  NativeFloatingFrameFixture() : NativeVoidFrameFixture(Arch::AArch64) {
    Med.ReturnType = High.ReturnType = NdType::makeInt(8);
    High.Body[0].RetVal = HighExpr::makeConst(42, 8);
    Med.Blocks[0].StartAddr = Low.Blocks[0].StartAddr = Med.Entry;
    // A source argument may occupy x3 without occupying earlier integer
    // argument registers. Changing the result ABI must not repack this input.
    Med.Params[0].RegOff = a64reg::X3;
    Section Text;
    Text.VA = Med.Entry;
    Text.Size = Text.FileSz = Image.Segments[0].FileSz;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    Image.Sections.push_back(std::move(Text));
    Segment Data;
    Data.VA = ImportSlot;
    Data.Size = Data.FileSz = 8;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(8);
    Image.Segments.push_back(std::move(Data));
    constexpr auto Provider =
        "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation";
    Image.ImportPtrSlots[ImportSlot] = "_CFStringGetDoubleValue";
    Image.DyldBindSlots[ImportSlot] = {"_CFStringGetDoubleValue", 0, Provider,
                                       false};
    Image.DynInfo.NeededLibs.push_back(Provider);
    const uint32_t Words[] = {0xd0000010, 0xf9400210, 0xd61f0200};
    for (unsigned I = 0; I != 3; ++I)
      for (unsigned J = 0; J != 4; ++J)
        Image.Segments[0].Data[0x1080 - Med.Entry + I * 4 + J] =
            Words[I] >> (J * 8);
    auto &LowOps = Low.Blocks[0].Ops;
    LowOps.insert(LowOps.begin() + CallIndex,
                  op(NdOp::COPY, NdVar::reg(a64reg::X0, 8),
                     {NdVar::reg(a64reg::X3, 8)}, 0x100c));
    ++CallIndex;
    ++RestoreIndex;
    const auto Current = buildObjCSourceCallHints(Image, Low);
    const auto Binding = Current.find(0x1010);
    EXPECT_NE(Binding, Current.end());
    if (Binding == Current.end())
      return;
    auto &Call = Med.Blocks[0].Ops[0];
    Call.SourceCallHint =
        std::make_shared<const SourceCallTypeHint>(Binding->second);
    Call.CallSiteId = 1;
    Call.Inputs[1] = Med.Params[0];
    Call.Output.Kind = MedVar::Reg;
    Call.Output.Id = 10;
    Call.Output.SSAVer = 1;
    Call.Output.TheArch = Arch::AArch64;
    Call.Output.RegOff = a64reg::V0;
    Call.Output.Size = 8;
    Med.Blocks[0].Ops.back().Addr = LowOps.back().Addr;
  }
};

TEST(NativeSourceHints, FloatingCandidateKeepsTheExistingArgumentCarrier) {
  NativeFloatingFrameFixture F;
  std::string Error;
  ASSERT_EQ(F.Med.ReturnType->Kind, NdTypeKind::Int);
  ASSERT_EQ(F.Med.ReturnType->Size, 8U);
  // Neither the provisional i64 result nor the floating value alone can
  // supply the required native frame proof.
  EXPECT_FALSE(F.infer(Error));
  const auto Hint = F.inferVoid(Error);
  ASSERT_TRUE(Hint) << Error;
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Float);
  EXPECT_EQ(Hint->ReturnType->Size, 8U);
  EXPECT_EQ(Hint->ReturnLocation.Kind, SourceABICarrierKind::FloatingRegister);
  EXPECT_EQ(Hint->ReturnLocation.RegisterOffset, a64reg::V0);
  EXPECT_EQ(Hint->ReturnLocation.ValueBytes, 8U);
  ASSERT_EQ(Hint->Parameters.size(), 1U);
  EXPECT_EQ(Hint->Parameters[0].Type->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Hint->Parameters[0].Location.Kind,
            SourceABICarrierKind::IntegerRegister);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X3);
  EXPECT_EQ(Hint->Parameters[0].Location.ValueBytes, 8U);
  EXPECT_EQ(F.Med.ReturnType->Kind, NdTypeKind::Int);
  EXPECT_EQ(F.High.ReturnType->Kind, NdTypeKind::Int);
}

TEST(NativeSourceHints, FloatingCandidateStillRequiresCallAndFrameEvidence) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    NativeFloatingFrameFixture F;
    if (Mutation == 0)
      F.Med.Blocks[0].Ops[0].SourceCallHint.reset();
    else if (Mutation == 1)
      ++F.Low.Blocks[0].Ops[F.CallIndex].Seq;
    else if (Mutation == 2)
      F.Low.Blocks[0].Ops[F.CallIndex].Inputs[0].Offset += 4;
    else
      F.Low.Blocks[0].Ops[F.RestoreIndex + 1].Opcode = NdOp::NOP;
    std::string Error;
    EXPECT_FALSE(F.inferVoid(Error)) << Mutation << ": " << Error;
  }
}

TEST(NativeSourceHints, FloatingCandidateNeverReplacesADefinedIntegerResult) {
  NativeFloatingFrameFixture F;
  MedOp Integer;
  Integer.Opcode = NdOp::COPY;
  Integer.Addr = 0x1014;
  Integer.Output = F.Med.Blocks[0].Ops[0].Output;
  Integer.Output.Id = 11;
  Integer.Output.RegOff = a64reg::X0;
  Integer.addInput(MedVar::makeConst(42, 8));
  F.Med.Blocks[0].Ops.insert(F.Med.Blocks[0].Ops.end() - 1, Integer);
  F.Low.Blocks[0].Ops.insert(
      F.Low.Blocks[0].Ops.begin() + F.RestoreIndex,
      F.op(NdOp::COPY, NdVar::reg(a64reg::X0, 8), {NdVar::cst(42, 8)}, 0x1014));
  std::string Error;
  const auto Hint = F.inferVoid(Error);
  ASSERT_TRUE(Hint) << Error;
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Int);
  EXPECT_EQ(Hint->ReturnType->Size, 8U);
  EXPECT_EQ(Hint->ReturnLocation.Kind, SourceABICarrierKind::IntegerRegister);
  EXPECT_EQ(Hint->ReturnLocation.RegisterOffset, a64reg::X0);
}

struct NativeMixedTerminalFixture : NativeVoidFrameFixture {
  static constexpr va_t FailureSlot = 0x3000;
  static constexpr va_t FailureStub = 0x10d0;
  static constexpr va_t FailureCall = 0x1040;

  NativeMixedTerminalFixture() : NativeVoidFrameFixture(Arch::AArch64) {
    useNativeVoidCallee();
    splitReturns();
    Low.Blocks[0].StartAddr = Med.Entry;
    Low.Blocks[1].StartAddr = 0x1020;
    Low.Blocks[2].StartAddr = FailureCall;
    auto Return = Med.Blocks[0].Ops.back();
    Return.Addr = 0x1024;
    Med.Blocks[0].StartAddr = Med.Entry;
    Med.Blocks[0].Ops.pop_back();
    Med.Blocks[0].Succs = {1, 2};
    MedBlock Normal;
    Normal.Id = 1;
    Normal.StartAddr = 0x1020;
    Normal.Preds = {0};
    Normal.Ops = {Return};
    MedBlock Failure;
    Failure.Id = 2;
    Failure.StartAddr = FailureCall;
    Failure.Preds = {0};
    Section Text;
    Text.VA = Med.Entry;
    Text.Size = Text.FileSz = Image.Segments[0].FileSz;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    Image.Sections.push_back(std::move(Text));
    Segment Data;
    Data.VA = FailureSlot;
    Data.Size = Data.FileSz = 8;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(8);
    Image.Segments.push_back(std::move(Data));
    Image.ImportPtrSlots[FailureSlot] = "___stack_chk_fail";
    Image.DyldBindSlots[FailureSlot] = {"___stack_chk_fail", 0,
                                        "/usr/lib/libSystem.B.dylib", false};
    Image.DynInfo.NeededLibs.push_back("/usr/lib/libSystem.B.dylib");
    // ADRP x16, 0x3000; LDR x16, [x16]; BR x16. The current loader must
    // independently connect the executable veneer to this exact import slot.
    const uint32_t Words[] = {0xd0000010, 0xf9400210, 0xd61f0200};
    for (unsigned I = 0; I != 3; ++I)
      for (unsigned J = 0; J != 4; ++J)
        Image.Segments[0].Data[FailureStub - Med.Entry + I * 4 + J] =
            Words[I] >> (J * 8);
    auto Hint = darwinRuntimeSourceCallHint(Image, FailureSlot);
    EXPECT_TRUE(Hint);
    if (!Hint)
      return;
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = FailureCall;
    Call.OriginSeq = 12;
    Call.CallSiteId = 2;
    Call.DoesNotReturn = true;
    Call.SourceCallHint =
        std::make_shared<const SourceCallTypeHint>(std::move(*Hint));
    Call.addInput(MedVar::makeConst(FailureStub, 8));
    Failure.Ops = {Call};
    Med.Blocks.push_back(std::move(Normal));
    Med.Blocks.push_back(std::move(Failure));
    auto NativeCall =
        op(NdOp::CALL, {}, {NdVar::cst(FailureStub, 8)}, FailureCall);
    NativeCall.Seq = Call.OriginSeq;
    Low.Blocks[2].Ops = {NativeCall};
  }

  void
  changeFailureHint(const std::function<void(SourceCallTypeHint &)> &Change) {
    auto &Call = Med.Blocks[2].Ops[0];
    auto Hint = std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
    Change(*Hint);
    Call.SourceCallHint = std::move(Hint);
  }

  NativeSourceCalls contracts() const {
    NativeSourceCalls Calls;
    for (const auto &B : Med.Blocks)
      for (const auto &Op : B.Ops) {
        if (Op.Opcode != NdOp::CALL || !Op.SourceCallHint)
          continue;
        NativeSourceCallContract Contract;
        Contract.Signature = &Op.SourceCallHint->Signature;
        if (Op.CallSiteId == 2)
          Contract.Termination =
              NativeSourceCallContract::TerminationKind::StackCheckFailure;
        Calls.emplace(NativeSourceCallKey{Op.Addr, Op.OriginSeq, Op.Opcode,
                                          Op.Inputs[0].ConstVal},
                      Contract);
      }
    return Calls;
  }
};

struct NativeMixedDictionaryViolationFixture : NativeMixedTerminalFixture {
  NativeMixedDictionaryViolationFixture() {
    constexpr auto Name = "$ss53KEY_TYPE_OF_DICTIONARY_VIOLATES_HASHABLE_"
                          "REQUIREMENTSys5NeverOypXpF";
    constexpr auto Module = "/usr/lib/swift/libswiftCore.dylib";
    Image.ImportPtrSlots[FailureSlot] = std::string("_") + Name;
    Image.DyldBindSlots[FailureSlot] = {std::string("_") + Name, 0, Module,
                                        false};
    Image.DynInfo.NeededLibs = {Module};
    auto Hint = swiftRuntimeSourceCallHint(Image, FailureSlot);
    EXPECT_TRUE(Hint);
    if (!Hint)
      return;
    auto &Call = Med.Blocks[2].Ops[0];
    Call.SourceCallHint =
        std::make_shared<const SourceCallTypeHint>(std::move(*Hint));
    Call.addInput(MedVar::makeConst(0x4000, 8));
  }

  NativeSourceCalls contracts() const {
    auto Calls = NativeMixedTerminalFixture::contracts();
    const auto Key = nativeSourceCallKey(Low.Blocks[2].Ops[0]);
    if (Key)
      Calls.at(*Key).Termination =
          NativeSourceCallContract::TerminationKind::SwiftDictionaryViolation;
    return Calls;
  }
};

TEST(NativeSourceHints, MixedSwiftDictionaryViolationPreservesNormalReturns) {
  NativeMixedDictionaryViolationFixture F;
  SourceFunctionTypeHint Expected;
  Expected.Origin = SourceFunctionTypeHint::OriginKind::SwiftSDK;
  Expected.ReturnType = NdType::makeVoid();
  Expected.Parameters = {{"arg0", NdType::makePtr(NdType::makeVoid())}};
  std::string ABIError;
  EXPECT_TRUE(assignDarwinSwiftSourceABI(Expected, Arch::AArch64, ABIError))
      << ABIError;
  EXPECT_TRUE(equalSourceABIs(F.Med.Blocks[2].Ops[0].SourceCallHint->Signature,
                              Expected));
  EXPECT_TRUE(restoresNativeSourceState(F.Low, Arch::AArch64, F.contracts()));
  std::string Error;
  const auto Hint = F.inferVoid(Error);
  ASSERT_TRUE(Hint) << Error;
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);

  F.Low.Blocks[1].Ops[1].Output = NdVar::reg(a64reg::X20, 8);
  EXPECT_FALSE(F.inferVoid(Error));
}

TEST(NativeSourceHints, MixedSwiftDictionaryViolationAllowsImmediateTrap) {
  NativeMixedDictionaryViolationFixture F;
  auto Trap =
      F.op(NdOp::INTRINSIC, {},
           {NdVar::cst(static_cast<uint64_t>(Intrinsic::Brk), 2)}, 0x1044);
  F.Low.Blocks[2].Ops.push_back(Trap);
  EXPECT_TRUE(restoresNativeSourceState(F.Low, Arch::AArch64, F.contracts()));
  F.Low.Blocks[2].Ops.back().Opcode = NdOp::NOP;
  EXPECT_FALSE(restoresNativeSourceState(F.Low, Arch::AArch64, F.contracts()));
}

TEST(NativeSourceHints, MixedSwiftDictionaryViolationRevalidatesImport) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    NativeMixedDictionaryViolationFixture F;
    if (Mutation == 0)
      F.Image.DyldBindSlots[F.FailureSlot].Module = "/tmp/foreign.dylib";
    else if (Mutation == 1)
      F.Image.DyldBindSlots[F.FailureSlot].Addend = 8;
    else if (Mutation == 2)
      F.Image.DyldBindSlots[F.FailureSlot].WeakImport = true;
    else if (Mutation == 3)
      F.changeFailureHint(
          [](auto &Hint) { Hint.Signature.Parameters.clear(); });
    else
      F.Med.Blocks[2].Ops[0].DoesNotReturn = false;
    std::string Error;
    EXPECT_FALSE(F.inferVoid(Error));
  }
}

TEST(NativeSourceHints, MixedStackFailureKeepsEveryNormalExitRestored) {
  NativeMixedTerminalFixture F;
  std::string Error;
  const auto Hint = F.inferVoid(Error);
  ASSERT_TRUE(Hint) << Error;
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
  EXPECT_FALSE(F.Med.DoesNotReturn);
  EXPECT_FALSE(F.High.DoesNotReturn);
  EXPECT_EQ(F.Med.ReturnType->Kind, NdTypeKind::Int);
  EXPECT_TRUE(restoresNativeSourceState(F.Low, Arch::AArch64, F.contracts()));

  const auto Original = F.Low.Blocks[1].Ops[1];
  F.Low.Blocks[1].Ops[1].Output = NdVar::reg(a64reg::X20, 8);
  EXPECT_FALSE(F.inferVoid(Error));
  F.Low.Blocks[1].Ops[1] = Original;
  // An additional valid normal exit cannot hide another un-restored exit.
  auto Bad = F.Low.Blocks[1];
  Bad.Id = 3;
  Bad.StartAddr = 0x1050;
  Bad.Ops[1].Output = NdVar::reg(a64reg::X20, 8);
  F.Low.Blocks.push_back(std::move(Bad));
  F.Low.Blocks[0].Succs.push_back(3);
  auto MedBad = F.Med.Blocks[1];
  MedBad.Id = 3;
  MedBad.StartAddr = 0x1050;
  F.Med.Blocks.push_back(std::move(MedBad));
  F.Med.Blocks[0].Succs.push_back(3);
  EXPECT_FALSE(F.inferVoid(Error));
}

TEST(NativeSourceHints, MixedStackFailureRevalidatesExactImportAndVeneer) {
  const std::array<std::function<void(NativeMixedTerminalFixture &)>, 12>
      Mutations = {{
          [](auto &F) {
            F.Image.DyldBindSlots[F.FailureSlot].Module = "/tmp/foreign.dylib";
          },
          [](auto &F) {
            F.Image.DyldBindSlots[F.FailureSlot].WeakImport = true;
          },
          [](auto &F) { F.Image.DyldBindSlots[F.FailureSlot].Addend = 8; },
          [](auto &F) { F.Image.DyldBindSlots.erase(F.FailureSlot); },
          [](auto &F) { F.Image.ImportPtrSlots[F.FailureSlot] = "_abort"; },
          [](auto &F) {
            F.Image.ConflictingImportStorageSlots.insert(F.FailureSlot);
          },
          [](auto &F) {
            F.Image.Segments[0].Data[F.FailureStub - F.Med.Entry + 8] ^= 4;
          },
          [](auto &F) { F.Med.Blocks[2].Ops[0].Inputs[0].ConstVal += 4; },
          [](auto &F) { ++F.Med.Blocks[2].Ops[0].OriginSeq; },
          [](auto &F) { F.Image.Arch = Arch::X64; },
          [](auto &F) { F.Image.DynInfo.NeededLibs.clear(); },
          [](auto &F) {
            F.Image.DynInfo.NeededLibs = {"/tmp/libSystem.B.dylib"};
          },
      }};
  for (size_t I = 0; I != Mutations.size(); ++I) {
    SCOPED_TRACE(I);
    NativeMixedTerminalFixture F;
    Mutations[I](F);
    std::string Error;
    EXPECT_FALSE(F.inferVoid(Error));
  }
}

TEST(NativeSourceHints, MixedStackFailureRejectsSignatureAndEffectDrift) {
  const std::array<std::function<void(SourceCallTypeHint &)>, 10> Mutations = {{
      [](auto &H) { H.DoesNotReturn = false; },
      [](auto &H) { H.TargetName = "abort"; },
      [](auto &H) { H.CallKind = SourceCallTypeHint::Kind::SwiftRuntimeCall; },
      [](auto &H) { H.WeakImport = true; },
      [](auto &H) { H.Signature.ReturnType = NdType::makeInt(8); },
      [](auto &H) {
        H.Signature.Parameters.push_back({"extra", NdType::makeInt(8)});
      },
      [](auto &H) {
        H.Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      },
      [](auto &H) { H.ReturnedArgument = 0; },
      [](auto &H) { H.NilTerminated.emplace(); },
      [](auto &H) { H.ByteCount = 8; },
  }};
  for (size_t I = 0; I != Mutations.size(); ++I) {
    SCOPED_TRACE(I);
    NativeMixedTerminalFixture F;
    F.changeFailureHint(Mutations[I]);
    std::string Error;
    EXPECT_FALSE(F.inferVoid(Error));
  }
  NativeMixedTerminalFixture F;
  F.Med.Blocks[2].Ops[0].DoesNotReturn = false;
  std::string Error;
  EXPECT_FALSE(F.inferVoid(Error));
  F.Med.Blocks[2].Ops[0].DoesNotReturn = true;
  F.Med.Blocks[2].Ops[0].PreservesCallerSaved = true;
  EXPECT_FALSE(F.inferVoid(Error));
}

TEST(NativeSourceHints, MixedStackFailureRequiresAnActualTerminalSink) {
  const std::array<std::function<void(NativeMixedTerminalFixture &)>, 5>
      Mutations = {{
          [](auto &F) {
            F.Low.Blocks[2].Ops.push_back(F.op(NdOp::NOP, {}, {}, 0x1044));
          },
          [](auto &F) {
            F.Low.Blocks[2].Ops.push_back(F.Low.Blocks[1].Ops.back());
          },
          [](auto &F) {
            F.Low.Blocks[2].Succs = {1};
            F.Low.Blocks[1].Preds.push_back(2);
          },
          [](auto &F) {
            F.Low.Blocks[0].Succs = {2};
            F.Low.Blocks[1].Preds.clear();
          },
          [](auto &F) {
            F.Low.Blocks[0].Succs = {2};
            F.Low.Blocks.erase(F.Low.Blocks.begin() + 1);
          },
      }};
  for (size_t I = 0; I != Mutations.size(); ++I) {
    SCOPED_TRACE(I);
    NativeMixedTerminalFixture F;
    Mutations[I](F);
    std::string Error;
    EXPECT_FALSE(F.inferVoid(Error));
    EXPECT_FALSE(
        restoresNativeSourceState(F.Low, Arch::AArch64, F.contracts()));
  }
}

TEST(NativeSourceHints,
     MixedStackFailureStillChecksFrameTaintBeforeTermination) {
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    SCOPED_TRACE(Mutation);
    NativeMixedTerminalFixture F;
    auto &Ops = F.Low.Blocks[2].Ops;
    if (Mutation == 0)
      Ops.insert(Ops.begin(),
                 F.op(NdOp::STORE, {},
                      {NdVar::cst(0x4000, 8), NdVar::reg(a64reg::SP, 8)},
                      0x103c));
    else if (Mutation == 1)
      Ops.insert(Ops.begin(), F.op(NdOp::COPY, NdVar::reg(a64reg::SP, 8),
                                   {NdVar::cst(0, 8)}, 0x103c));
    else
      F.Low.Blocks[0].Ops.insert(F.Low.Blocks[0].Ops.begin() + F.CallIndex,
                                 F.op(NdOp::COPY, NdVar::reg(a64reg::X0, 8),
                                      {NdVar::reg(a64reg::SP, 8)}, 0x100c));
    std::string Error;
    EXPECT_FALSE(F.inferVoid(Error));
  }
}

TEST(NativeSourceHints,
     MixedStackFailureDoesNotAcceptOtherTerminationContracts) {
  NativeMixedTerminalFixture F;
  for (auto Kind :
       {NativeSourceCallContract::TerminationKind::None,
        NativeSourceCallContract::TerminationKind::RuntimeEntry,
        static_cast<NativeSourceCallContract::TerminationKind>(99)}) {
    auto Calls = F.contracts();
    const auto Key = nativeSourceCallKey(F.Low.Blocks[2].Ops[0]);
    ASSERT_TRUE(Key);
    Calls.at(*Key).Termination = Kind;
    EXPECT_FALSE(restoresNativeSourceState(F.Low, Arch::AArch64, Calls));
  }
  auto Calls = F.contracts();
  const auto Key = nativeSourceCallKey(F.Low.Blocks[2].Ops[0]);
  ASSERT_TRUE(Key);
  Calls.at(*Key).ReadOnlyFrameParameters.emplace(0, 8);
  EXPECT_FALSE(restoresNativeSourceState(F.Low, Arch::AArch64, Calls));
}

struct NativeIncomingStackFixture : NativeVoidFrameFixture {
  int64_t IncomingOffset;
  size_t AddressIndex;
  size_t LoadIndex;

  explicit NativeIncomingStackFixture(int64_t Offset = 0)
      : NativeVoidFrameFixture(Arch::AArch64), IncomingOffset(Offset) {
    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.Id =
        getTargetRegInfo(Arch::AArch64).IntParamRegs.size() + Offset / 8;
    Parameter.RegOff = kNoParamReg;
    Parameter.Size = 8;
    Parameter.TheArch = Arch::AArch64;
    Med.Params = {Parameter};
    Med.TypedParams = {{"incoming", NdType::makeInt(8)}};
    High.Params = {{"incoming", NdType::makeInt(8)}};
    Med.Blocks[0].Ops[0].Inputs[1] = Parameter;
    useNativeVoidCallee();

    AddressIndex = CallIndex;
    LoadIndex = CallIndex + 1;
    const auto Address = NdVar::tmp(TmpBase + 128, 8);
    auto &Ops = Low.Blocks[0].Ops;
    Ops.insert(
        Ops.begin() + CallIndex,
        {op(NdOp::INT_ADD, Address,
            {NdVar::reg(a64reg::SP, 8), NdVar::cst(FrameBytes + Offset, 8)},
            0x100c),
         op(NdOp::LOAD, NdVar::reg(a64reg::X0, 8), {Address}, 0x100c)});
    CallIndex += 2;
    RestoreIndex += 2;
  }

  SourceFunctionTypeHint entrySignature() const {
    SourceFunctionTypeHint Result;
    Result.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Result.Architecture = Arch::AArch64;
    Result.HasExplicitABI = true;
    Result.ReturnType = NdType::makeVoid();
    Result.Parameters = {{"incoming",
                          NdType::makeInt(8),
                          {SourceABICarrierKind::Stack, 0, IncomingOffset, 8}}};
    return Result;
  }

  NativeSourceCalls calls() const {
    const auto Key = nativeSourceCallKey(Low.Blocks[0].Ops[CallIndex]);
    EXPECT_TRUE(Key);
    NativeSourceCallContract Contract;
    Contract.Signature = &Med.Blocks[0].Ops[0].SourceCallHint->Signature;
    return Key ? NativeSourceCalls{{*Key, Contract}} : NativeSourceCalls{};
  }
};

TEST(NativeSourceHints, IncomingStackReadsRequireCompleteObservedSlots) {
  for (int64_t Offset : {0, 16})
    for (bool Repeat : {false, true}) {
      SCOPED_TRACE(Offset);
      SCOPED_TRACE(Repeat);
      NativeIncomingStackFixture F(Offset);
      if (Repeat) {
        // Incoming values may be read again after a returning call. They are
        // external inputs, not private spill facts surviving that call.
        auto Address = F.Low.Blocks[0].Ops[F.AddressIndex];
        auto Load = F.Low.Blocks[0].Ops[F.LoadIndex];
        auto Call = F.Low.Blocks[0].Ops[F.CallIndex];
        Address.Addr = Load.Addr = Call.Addr = 0x1014;
        ++Call.Seq;
        F.Low.Blocks[0].Ops.insert(F.Low.Blocks[0].Ops.begin() + F.RestoreIndex,
                                   {Address, Load, Call});
        auto MedCall = F.Med.Blocks[0].Ops[0];
        MedCall.Addr = Call.Addr;
        MedCall.OriginSeq = Call.Seq;
        F.Med.Blocks[0].Ops.insert(F.Med.Blocks[0].Ops.begin() + 1, MedCall);
      }
      std::string Error;
      const auto Hint = F.inferVoid(Error);
      ASSERT_TRUE(Hint) << Error;
      EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
      ASSERT_EQ(Hint->Parameters.size(), 1U);
      EXPECT_EQ(Hint->Parameters[0].Location.Kind, SourceABICarrierKind::Stack);
      EXPECT_EQ(Hint->Parameters[0].Location.EntryStackOffset, Offset);
      EXPECT_EQ(Hint->Parameters[0].Location.ValueBytes, 8U);
      EXPECT_EQ(F.Med.Params[0].RegOff, kNoParamReg);
    }
}

TEST(NativeSourceHints, IncomingStackPrefixReadsRequireTheirOwnCompleteLoad) {
  for (uint16_t Width : {1, 2, 4})
    for (bool PrefixFirst : {false, true})
      for (bool OtherSlot : {false, true}) {
        SCOPED_TRACE(Width);
        SCOPED_TRACE(PrefixFirst);
        SCOPED_TRACE(OtherSlot);
        NativeIncomingStackFixture F(16);
        auto &Ops = F.Low.Blocks[0].Ops;
        const auto Address = NdVar::tmp(TmpBase + 256, 8);
        const auto Offset = F.IncomingOffset + (OtherSlot ? 8 : 0);
        Ops.insert(
            Ops.begin() + F.LoadIndex + (PrefixFirst ? 0 : 1),
            {NativeVoidFrameFixture::op(NdOp::INT_ADD, Address,
                                        {NdVar::reg(a64reg::SP, 8),
                                         NdVar::cst(F.FrameBytes + Offset, 8)},
                                        0x100c),
             NativeVoidFrameFixture::op(NdOp::LOAD,
                                        NdVar::tmp(TmpBase + 272, Width),
                                        {Address}, 0x100c)});
        F.LoadIndex += PrefixFirst ? 2 : 0;
        F.CallIndex += 2;
        F.RestoreIndex += 2;
        auto Signature = F.entrySignature();
        if (OtherSlot) {
          auto Parameter = Signature.Parameters.front();
          Parameter.Location.EntryStackOffset = Offset;
          Signature.Parameters.push_back(std::move(Parameter));
        }
        EXPECT_EQ(restoresNativeSourceState(F.Low, Arch::AArch64, F.calls(),
                                            nullptr, &Signature),
                  !OtherSlot);
        if (!OtherSlot) {
          std::string Error;
          EXPECT_TRUE(F.inferVoid(Error)) << Error;
          // Repeated prefix observations cannot collectively pretend that
          // this slot's otherwise unobserved upper bytes were read.
          Ops[F.LoadIndex].Output.Size = Width;
          EXPECT_FALSE(restoresNativeSourceState(
              F.Low, Arch::AArch64, F.calls(), nullptr, &Signature));
        }
      }
}

TEST(NativeSourceHints, IncomingStackReadsRejectMissingOrPartialEvidence) {
  for (unsigned Mutation = 0; Mutation != 14; ++Mutation) {
    SCOPED_TRACE(Mutation);
    NativeIncomingStackFixture F(16);
    auto &Ops = F.Low.Blocks[0].Ops;
    switch (Mutation) {
    case 0:
      F.Med.Params.clear();
      F.Med.TypedParams.clear();
      F.High.Params.clear();
      F.Med.Blocks[0].Ops[0].Inputs[1] = MedVar::makeConst(1, 8);
      break;
    case 1:
      Ops[F.LoadIndex].Output.Size = 4;
      break;
    case 2:
      Ops[F.AddressIndex].Inputs[1].Offset += 8;
      break;
    case 3:
      Ops[F.LoadIndex] = NativeVoidFrameFixture::op(
          NdOp::STORE, {}, {Ops[F.LoadIndex].Inputs[0], NdVar::cst(42, 8)},
          0x100c);
      break;
    case 4:
      F.Med.Params.push_back(F.Med.Params[0]);
      F.Med.TypedParams.push_back(F.Med.TypedParams[0]);
      F.High.Params.push_back(F.High.Params[0]);
      break;
    case 5:
      F.Med.Params[0].Size = F.Med.Blocks[0].Ops[0].Inputs[1].Size = 4;
      F.Med.TypedParams[0].Type = F.High.Params[0].Type = NdType::makeInt(4);
      break;
    case 6:
      F.Med.TypedParams[0].Type = F.High.Params[0].Type = NdType::makeFloat(8);
      break;
    case 7:
      F.Med.Params[0].Id = F.Med.Blocks[0].Ops[0].Inputs[1].Id = 7;
      break;
    case 8:
      // A sixteen-byte read cannot borrow permission for one eight-byte slot.
      Ops[F.LoadIndex].Output.Size = 16;
      break;
    case 9:
      Ops[F.LoadIndex].MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 10:
      Ops[F.LoadIndex].MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 11:
      ++Ops[F.AddressIndex].Inputs[1].Offset;
      break;
    case 12:
      F.Med.TypedParams.clear();
      break;
    case 13:
      F.High.Params.clear();
      break;
    }
    std::string Error;
    EXPECT_FALSE(F.inferVoid(Error)) << Error;
    EXPECT_FALSE(Error.empty());
  }
}

TEST(NativeSourceHints, IncomingStackReadsCannotRestoreMachineState) {
  for (const auto Register :
       {a64reg::X19, a64reg::X29, a64reg::X30, a64reg::SP})
    for (unsigned UnknownAlias = 0; UnknownAlias != 3; ++UnknownAlias) {
      SCOPED_TRACE(Register);
      SCOPED_TRACE(UnknownAlias);
      NativeIncomingStackFixture F;
      auto &Ops = F.Low.Blocks[0].Ops;
      // First restore the real private spills. A later incoming load cannot
      // replace any restored identity, even when its slot and width are valid.
      const auto Address = NdVar::tmp(TmpBase + 256, 8);
      const auto Derive =
          UnknownAlias == 1
              ? NativeVoidFrameFixture::op(
                    NdOp::INT_ADD, Address,
                    {NdVar::reg(a64reg::SP, 8), NdVar::reg(a64reg::X1, 8)},
                    0x1024)
              : NativeVoidFrameFixture::op(NdOp::COPY, Address,
                                           {NdVar::reg(a64reg::SP, 8)}, 0x1024);
      Ops.insert(Ops.end() - 1,
                 {Derive, NativeVoidFrameFixture::op(NdOp::LOAD,
                                                     NdVar::reg(Register, 8),
                                                     {Address}, 0x1024)});
      if (UnknownAlias == 2)
        Ops[Ops.size() - 2].Inputs[0].Size = 4;
      const auto Signature = F.entrySignature();
      EXPECT_FALSE(restoresNativeSourceState(F.Low, Arch::AArch64, F.calls(),
                                             nullptr, &Signature));
      std::string Error;
      EXPECT_FALSE(F.inferVoid(Error)) << Error;
    }
}

TEST(NativeSourceHints, UnknownIncomingAliasDoesNotAcquireStackSlotAuthority) {
  for (bool PartialAddress : {false, true}) {
    SCOPED_TRACE(PartialAddress);
    NativeIncomingStackFixture F;
    if (PartialAddress)
      F.Low.Blocks[0].Ops[F.LoadIndex].Inputs[0].Size = 4;
    else
      F.Low.Blocks[0].Ops[F.AddressIndex].Inputs[1] = NdVar::reg(a64reg::X1, 8);
    // A read through an unresolved address already produces Unknown in the
    // preservation analysis. No incoming-slot permission is needed or inferred;
    // source body validity remains a separate publication requirement.
    EXPECT_TRUE(restoresNativeSourceState(F.Low, Arch::AArch64, F.calls()));
    const auto Signature = F.entrySignature();
    EXPECT_TRUE(restoresNativeSourceState(F.Low, Arch::AArch64, F.calls(),
                                          nullptr, &Signature));
  }
}

TEST(NativeSourceHints, IncomingStackSignatureUsesPhysicalOffsets) {
  for (unsigned Mutation = 0; Mutation != 14; ++Mutation) {
    SCOPED_TRACE(Mutation);
    NativeIncomingStackFixture F(16);
    auto Signature = F.entrySignature();
    const SourceFunctionTypeHint *Entry = &Signature;
    switch (Mutation) {
    case 0:
      break; // The first logical parameter occupies the physical SP+16 slot.
    case 1:
      Entry = nullptr;
      break;
    case 2:
      Signature.Parameters.clear();
      break;
    case 3:
      Signature.Parameters[0].Location.EntryStackOffset = 0;
      break;
    case 4:
      Signature.Parameters[0].Location.EntryStackOffset = 17;
      break;
    case 5:
      Signature.Parameters.push_back(Signature.Parameters[0]);
      break;
    case 6:
      Signature.Parameters[0].Type = NdType::makeInt(4);
      Signature.Parameters[0].Location.ValueBytes = 4;
      break;
    case 7:
      Signature.Parameters[0].Type = NdType::makeFloat(8);
      break;
    case 8:
      Signature.HasExplicitABI = false;
      break;
    case 9:
      Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftSDK;
      break;
    case 10:
      Signature.Architecture = Arch::X64;
      break;
    case 11:
      Signature.Parameters[0].Location.EntryStackOffset = INT64_MAX;
      break;
    case 12:
      Signature.Parameters[0].Location.EntryStackOffset = -8;
      break;
    case 13:
      Signature.Parameters[0].Location.RegisterOffset = a64reg::X1;
      break;
    }
    EXPECT_EQ(restoresNativeSourceState(F.Low, Arch::AArch64, F.calls(),
                                        nullptr, Entry),
              Mutation == 0);
  }
}

std::shared_ptr<SourceCallTypeHint>
useVoidRecordCallee(NativeVoidFixture &Fixture, const TypeRef &Record) {
  auto &Call = Fixture.Med.Blocks[0].Ops[0];
  auto Binding = std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
  Binding->CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
  Binding->Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinRuntime;
  Binding->Signature.Parameters = {{"record", Record}};
  std::string Error;
  EXPECT_TRUE(
      assignDarwinFixedSourceABI(Binding->Signature, Fixture.Image.Arch, Error))
      << Error;
  Fixture.Med.Params.clear();
  Fixture.Med.TypedParams.clear();
  Fixture.High.Params.clear();
  Call.NumInputs = 1;
  for (const auto &Physical : sourceABIParameters(Binding->Signature))
    Call.addInput(MedVar::makeConst(17, Physical.Type->Size));
  Call.SourceCallHint = Binding;
  return Binding;
}

TEST(NativeSourceHints, VoidRecordsCheckEveryPhysicalRegisterMember) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Shape = 0; Shape < 4; ++Shape)
      for (unsigned Path = 0; Path < 3; ++Path) {
        SCOPED_TRACE(unsigned(Architecture));
        SCOPED_TRACE(Shape);
        SCOPED_TRACE(Path);
        // Floating records currently have an authoritative ABI only on arm64.
        if (Architecture == Arch::X64 && Shape != 2)
          continue;
        const auto Record =
            Shape == 0   ? NdType::makeStruct(
                               {NdType::makeFloat(8), NdType::makeFloat(8)})
            : Shape == 1 ? NdType::makeStruct(
                               {NdType::makeFloat(4), NdType::makeFloat(4)})
            : Shape == 2
                ? NdType::makeStruct(
                      {NdType::makeInt(8), NdType::makePtr(NdType::makeVoid())})
                : NdType::makeStruct(
                      {NdType::makeFloat(8), NdType::makeFloat(8),
                       NdType::makeFloat(8), NdType::makeFloat(8)});
        // Exercise ordinary framed calls, the general tail-call proof, and
        // the narrower leaf fallback independently. The fallback's harmless
        // intrinsic is deliberately unsupported by the general frame proof.
        NativeVoidFixture F = Path == 0 ? NativeVoidFrameFixture(Architecture)
                                        : NativeVoidFixture(Architecture);
        const auto Binding = useVoidRecordCallee(F, Record);
        if (Path == 2) {
          LowOp Intrinsic;
          Intrinsic.Opcode = NdOp::INTRINSIC;
          Intrinsic.Addr = 0x1004;
          F.Low.Blocks[0].Ops.insert(F.Low.Blocks[0].Ops.begin(), Intrinsic);
        }
        std::string Error;
        const auto Hint = F.inferVoid(Error);
        ASSERT_TRUE(Hint) << Error;
        EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
        EXPECT_EQ(Hint->ReturnLocation.Kind, SourceABICarrierKind::None);
        EXPECT_TRUE(Hint->Parameters.empty());

        const auto Physicals = sourceABIParameters(Binding->Signature);
        ASSERT_EQ(Physicals.size(), Shape == 3 ? 4U : 2U);
        // Any byte in either member can carry a frame address, including a
        // non-leading FP lane. Clear one interior byte to ensure other-byte
        // taint cannot be erased by a partial overwrite.
        for (const auto &Physical : Physicals)
          for (bool PartialClear : {false, true}) {
            auto Escaping = F;
            auto &Ops = Escaping.Low.Blocks[0].Ops;
            const auto Call =
                std::find_if(Ops.begin(), Ops.end(),
                             [](auto &Op) { return Op.Opcode == NdOp::CALL; });
            ASSERT_NE(Call, Ops.end());
            const auto Index = Call - Ops.begin();
            const auto Register = Physical.Location.RegisterOffset;
            LowOp Derive = NativeVoidFrameFixture::op(
                NdOp::COPY, NdVar::reg(Register, 8),
                {NdVar::reg(getTargetRegInfo(Architecture).StackPointer, 8)},
                0x100c);
            Ops.insert(Ops.begin() + Index, Derive);
            if (PartialClear)
              Ops.insert(Ops.begin() + Index + 1,
                         NativeVoidFrameFixture::op(
                             NdOp::COPY, NdVar::reg(Register + 1, 1),
                             {NdVar::cst(0, 1)}, 0x100c));
            EXPECT_FALSE(Escaping.inferVoid(Error)) << Error;
          }
      }
}

TEST(NativeSourceHints, VoidRecordsRejectStaleAndBorrowedMemberContracts) {
  for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    NativeVoidFrameFixture F(Arch::AArch64);
    auto Binding = useVoidRecordCallee(
        F, NdType::makeStruct({NdType::makeInt(8), NdType::makeInt(8)}));
    auto &Parameter = Binding->Signature.Parameters[0];
    switch (Mutation) {
    case 0:
      Parameter.Components.pop_back();
      break;
    case 1:
      Parameter.Components[1] = Parameter.Components[0];
      break;
    case 2:
      Parameter.Components[1].ValueBytes = 4;
      break;
    case 3:
      ++F.Low.Blocks[0].Ops[F.CallIndex].Seq;
      break;
    case 4:
      F.Med.Blocks[0].Ops[0].addInput(MedVar::makeConst(31, 8));
      break;
    case 5:
      --F.Med.Blocks[0].Ops[0].NumInputs;
      break;
    case 6:
    case 7:
      // The super2 certificate for source parameter zero only borrows a
      // scalar pointer. Its index must not authorize either record member.
      Binding->CallKind = SourceCallTypeHint::Kind::ObjCSuper2;
      Binding->Signature.Origin =
          SourceFunctionTypeHint::OriginKind::ObjCRuntime;
      F.Low.Blocks[0].Ops.insert(
          F.Low.Blocks[0].Ops.begin() + F.CallIndex,
          NativeVoidFrameFixture::op(
              NdOp::COPY,
              NdVar::reg(Parameter.Components[Mutation - 6].RegisterOffset, 8),
              {NdVar::reg(a64reg::SP, 8)}, 0x100c));
      break;
    }
    std::string Error;
    EXPECT_FALSE(F.inferVoid(Error)) << Error;
    EXPECT_FALSE(Error.empty());
  }
}

struct NativeOutgoingStackFixture : NativeVoidFrameFixture {
  size_t ArgumentStore;
  std::shared_ptr<SourceCallTypeHint> Binding;

  explicit NativeOutgoingStackFixture(Arch Architecture = Arch::AArch64)
      : NativeVoidFrameFixture(Architecture) {
    const auto &TRI = getTargetRegInfo(Architecture);
    // Reserve a separate outgoing word below all saved-register identities.
    FrameBytes += 16;
    auto &Ops = Low.Blocks[0].Ops;
    for (auto &Op : Ops) {
      if ((Op.Opcode == NdOp::INT_SUB || Op.Opcode == NdOp::INT_ADD) &&
          Op.Output == NdVar::reg(TRI.StackPointer, 8))
        Op.Inputs[1] = NdVar::cst(FrameBytes, 8);
      else if (Op.Opcode == NdOp::INT_ADD &&
               Op.Output == NdVar::tmp(TmpBase, 8))
        Op.Inputs[1].Offset += 16;
    }
    ArgumentStore = CallIndex;
    Ops.insert(Ops.begin() + CallIndex,
               op(NdOp::STORE, {},
                  {NdVar::reg(TRI.StackPointer, 8), NdVar::cst(0x7777, 8)},
                  0x100c));
    ++CallIndex;
    ++RestoreIndex;
    auto &Call = Med.Blocks[0].Ops[0];
    Binding = std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
    Binding->Signature.Parameters.push_back(
        {"word", NdType::makeInt(8), {SourceABICarrierKind::Stack, 0, 0, 8}});
    Call.SourceCallHint = Binding;
    Call.addInput(MedVar::makeConst(0x7777, 8));
  }
};

TEST(NativeSourceHints, RecordBorrowingKeepsOriginalScalarParameterIndexes) {
  for (bool Floating : {false, true})
    for (bool Writable : {false, true})
      for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
        SCOPED_TRACE(Floating);
        SCOPED_TRACE(Writable);
        SCOPED_TRACE(Mutation);
        NativeOutgoingStackFixture F;
        auto &Signature = F.Binding->Signature;
        const auto Member =
            Floating ? NdType::makeFloat(8) : NdType::makeInt(8);
        Signature.Parameters = {
            {"record", NdType::makeStruct({Member, Member})},
            {"scratch", NdType::makePtr(NdType::makeVoid())}};
        std::string Error;
        ASSERT_TRUE(assignDarwinFixedSourceABI(Signature, Arch::AArch64, Error))
            << Error;
        const auto Key = nativeSourceCallKey(F.Low.Blocks[0].Ops[F.CallIndex]);
        ASSERT_TRUE(Key);
        NativeSourceCallContract Contract;
        Contract.Signature = &Signature;
        auto &Borrowed = Writable ? Contract.WritableFrameParameters
                                  : Contract.ReadOnlyFrameParameters;
        // Logical parameter 1 is physical member 2 after a two-member record.
        Borrowed.emplace(Mutation == 1 ? 2 : 1, 8);
        auto &Ops = F.Low.Blocks[0].Ops;
        Ops.insert(
            Ops.begin() + F.CallIndex,
            NativeVoidFrameFixture::op(
                NdOp::COPY,
                NdVar::reg(Signature.Parameters[1].Location.RegisterOffset, 8),
                {NdVar::reg(a64reg::SP, 8)}, 0x100c));
        if (Mutation == 2 || Mutation == 3) {
          Borrowed.emplace(0, 8);
          Ops.insert(Ops.begin() + F.CallIndex,
                     NativeVoidFrameFixture::op(
                         NdOp::COPY,
                         NdVar::reg(Signature.Parameters[0]
                                        .Components[Mutation - 2]
                                        .RegisterOffset,
                                    8),
                         {NdVar::reg(a64reg::SP, 8)}, 0x100c));
        }
        if (Mutation == 4)
          Signature.Parameters[0].Components.pop_back();
        NativeSourceCalls Calls{{*Key, Contract}};
        EXPECT_EQ(restoresNativeSourceState(F.Low, Arch::AArch64, Calls),
                  Mutation == 0);
      }
}

namespace {
struct ValueBufferEffectFixture : immutable_native_call_test::Fixture {
  static constexpr va_t Projector = immutable_native_call_test::Target;
  static constexpr va_t CallSite = immutable_native_call_test::Call;
  ValueBufferEffectFixture() {
    using namespace immutable_native_call_test;
    using runtime_function_address_test::Slot;
    Image.ImportPtrSlots[Slot] = "_swift_makeBoxUnique";
    Image.DyldBindSlots[Slot].Name = "_swift_makeBoxUnique";
    Signature.ReturnType = NdType::makeInt(8);
    Signature.Parameters = {{"buffer", NdType::makePtr(NdType::makeVoid())},
                            {"metadata", NdType::makePtr(NdType::makeVoid())}};
    std::string Error;
    EXPECT_TRUE(assignDarwinScalarSourceABI(Signature, Arch::AArch64, Error));
    const uint32_t Words[] = {0xf85f8028, 0xb9405108, 0x368800e8, 0xa9bf7bfd,
                              0x910003fd, 0x92401d02, 0x97ffffba, 0xaa0103e0,
                              0xa8c17bfd, 0xd65f03c0};
    for (size_t I = 0; I < std::size(Words); ++I)
      word((Projector - Entry) / 4 + I, Words[I]);
    Image.Symbols.back().Size = sizeof(Words);
    // Keep the original caller's full decoder evidence and replace just its
    // indirect call with an ordinary BL to this concrete code entry.
    word((CallSite - Entry) / 4, 0x94000000 | ((Projector - CallSite) / 4));
    run();
  }
};
} // namespace

TEST(NativeSourceHints, ValueBufferEffectRequiresCurrentCodeImportAndCallABI) {
  ValueBufferEffectFixture F;
  ASSERT_TRUE(F.Result.Success) << F.Result.Error;
  ASSERT_TRUE(F.low());
  const auto Effects =
      swiftValueBufferProjectionEffects(F.Image, F.Projector, F.Signature);
  ASSERT_TRUE(Effects);
  EXPECT_TRUE(Effects->ReadOnlyFrameParameters.empty());
  EXPECT_EQ(Effects->WritableFrameParameters.at(0), 24U);
  ASSERT_TRUE(Effects->ReturnFrameOrExternal);
  EXPECT_EQ(Effects->ReturnFrameOrExternal->Parameter, 0U);
  EXPECT_EQ(Effects->ReturnFrameOrExternal->Bytes, 24U);
  const SourceCallOccurrenceKey Site{F.CallSite, 1, NdOp::CALL, F.Projector};
  EXPECT_EQ(swiftValueBufferCallEffects(F.Image, *F.low(), Site, F.Signature),
            Effects);
  // Names are diagnostic only; the immutable body and import carry the proof.
  auto Renamed = F.Image;
  Renamed.Symbols.back().Name = "unrelated_projector_name";
  EXPECT_EQ(
      swiftValueBufferProjectionEffects(Renamed, F.Projector, F.Signature),
      Effects);
  for (unsigned Mutation = 0; Mutation < 25; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = F.Image;
    auto Signature = F.Signature;
    if (Mutation < 10)
      Image.Segments[0].Data[F.Projector - immutable_native_call_test::Entry +
                             Mutation * 4] ^= 1;
    switch (Mutation) {
    case 10:
      Image.DyldBindSlots[runtime_function_address_test::Slot].WeakImport =
          true;
      break;
    case 11:
      Image.DyldBindSlots[runtime_function_address_test::Slot].Module =
          "/tmp/libswiftCore.dylib";
      break;
    case 12:
      Image.ImportPtrSlots[runtime_function_address_test::Slot] =
          "_swift_release";
      break;
    case 13:
      Image.DyldBindSlots.clear();
      break;
    case 14:
      Image.Segments[0].Flags = SegmentFlags::Readable |
                                SegmentFlags::Writable |
                                SegmentFlags::Executable;
      break;
    case 15:
      Image.Symbols.push_back({"interior", F.Projector + 8, 4, true});
      break;
    case 16:
      Image.Arch = Arch::X64;
      break;
    case 17:
      Image.IsRelocatable = true;
      break;
    case 18:
      Signature.Parameters[0].Location.RegisterOffset = a64reg::X2;
      break;
    case 19:
      Signature.Parameters[1].Type = NdType::makeInt(8);
      break;
    case 20:
      Signature.ReturnLocation.RegisterOffset = a64reg::X1;
      break;
    case 21:
      Signature.Parameters.pop_back();
      break;
    case 22:
      Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
      break;
    case 23:
      Signature.ReturnType = NdType::makeInt(4);
      Signature.ReturnLocation.ValueBytes = 4;
      break;
    case 24:
      Image.Segments[0].Data[immutable_native_call_test::Veneer -
                             immutable_native_call_test::Entry] ^= 1;
      break;
    }
    EXPECT_FALSE(
        swiftValueBufferProjectionEffects(Image, F.Projector, Signature));
  }
  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = F.Image;
    auto Low = *F.low();
    auto Key = Site;
    if (Mutation == 0)
      Image.Segments[0].Data[F.CallSite - immutable_native_call_test::Entry] ^=
          1;
    if (Mutation == 1)
      ++Key.Sequence;
    if (Mutation == 2)
      Low.Blocks[0].InstructionBoundaries.clear();
    if (Mutation == 3)
      ++Low.DecodedInstructionCount;
    if (Mutation == 4)
      for (auto &B : Low.Blocks)
        for (auto &Op : B.Ops)
          if (Op.Addr == F.CallSite && Op.Opcode == NdOp::CALL)
            Op.Inputs[0].Offset += 4;
    EXPECT_FALSE(swiftValueBufferCallEffects(Image, Low, Key, F.Signature));
  }
}

TEST(NativeSourceHints, ValueBufferInferenceRejectsStaleScalarCallEvidence) {
  for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ValueBufferEffectFixture F;
    ASSERT_TRUE(F.Result.Success) << F.Result.Error;
    ASSERT_TRUE(F.med());
    ASSERT_TRUE(F.high());
    ASSERT_TRUE(isSwiftValueBufferProjection(F.Image, F.Projector));
    if (Mutation == 1)
      F.word((F.CallSite - immutable_native_call_test::Entry) / 4, 0xd503201f);
    for (auto &Block : F.med()->Blocks)
      for (auto &Op : Block.Ops) {
        if (Op.Addr != F.CallSite || Op.Opcode != NdOp::CALL)
          continue;
        ASSERT_TRUE(Op.SourceCallHint);
        auto Binding = *Op.SourceCallHint;
        if (Mutation == 2)
          Binding.Signature.Parameters[0].Type = NdType::makeInt(8);
        if (Mutation == 3)
          Binding.TargetAddress += 4;
        if (Mutation == 4)
          ++Op.OriginSeq;
        if (Mutation == 5)
          Op.PreservesCallerSaved = true;
        if (Mutation == 6)
          Op.DoesNotReturn = true;
        if (Mutation == 7)
          --Op.NumInputs;
        Op.SourceCallHint = std::make_shared<const SourceCallTypeHint>(Binding);
      }
    const auto Audit =
        std::find_if(F.Result.FunctionAudits.begin(),
                     F.Result.FunctionAudits.end(), [](const auto &A) {
                       return A.Entry == immutable_native_call_test::Entry;
                     });
    ASSERT_NE(Audit, F.Result.FunctionAudits.end());
    std::string Reason;
    const auto Hint = inferNativeSourceTypeHint(F.Image, *F.med(), *F.high(),
                                                *Audit, Reason, F.low());
    EXPECT_EQ(bool(Hint), Mutation == 0) << Reason;
  }
}

// The projector may return its borrowed base or unrelated heap storage. No
// result byte can certify an exact stack location or a saved-register value.
struct NativeFrameAliasFixture : NativeVoidFrameFixture {
  SourceFunctionTypeHint Project, Consume;
  NativeSourceCalls Calls;
  size_t ConsumeIndex;

  NativeFrameAliasFixture() : NativeVoidFrameFixture(Arch::AArch64) {
    FrameBytes += 32;
    auto &Ops = Low.Blocks[0].Ops;
    for (auto &Op : Ops) {
      if ((Op.Opcode == NdOp::INT_SUB || Op.Opcode == NdOp::INT_ADD) &&
          Op.Output == NdVar::reg(a64reg::SP, 8))
        Op.Inputs[1] = NdVar::cst(FrameBytes, 8);
      else if (Op.Opcode == NdOp::INT_ADD &&
               Op.Output == NdVar::tmp(TmpBase, 8))
        Op.Inputs[1].Offset += 32;
    }
    Ops.insert(Ops.begin() + CallIndex,
               op(NdOp::COPY, NdVar::reg(a64reg::X0, 8),
                  {NdVar::reg(a64reg::SP, 8)}, 0x100c));
    ++CallIndex;
    ConsumeIndex = CallIndex + 1;
    Ops.insert(Ops.begin() + ConsumeIndex,
               op(NdOp::CALL, NdVar::reg(a64reg::X0, 8),
                  {NdVar::cst(0x1090, 8)}, 0x1018));
    RestoreIndex += 2;
    Project.Origin = Consume.Origin =
        SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Project.ReturnType = NdType::makeInt(8);
    Project.Parameters = {{"buffer", NdType::makePtr(NdType::makeVoid())},
                          {"metadata", NdType::makePtr(NdType::makeVoid())}};
    Consume.ReturnType = NdType::makeVoid();
    Consume.Parameters = {{"context", NdType::makeInt(8)}};
    std::string Error;
    EXPECT_TRUE(assignDarwinScalarSourceABI(Project, Arch::AArch64, Error));
    EXPECT_TRUE(assignDarwinScalarSourceABI(Consume, Arch::AArch64, Error));
    NativeSourceCallContract ProjectCall, ConsumeCall;
    ProjectCall.Signature = &Project;
    ProjectCall.WritableFrameParameters.emplace(0, 24);
    ProjectCall.ReturnFrameOrExternal = SourceFrameReturnAlias{0, 24};
    ConsumeCall.Signature = &Consume;
    ConsumeCall.ReadOnlyFrameParameters.emplace(0, 8);
    Calls.emplace(*nativeSourceCallKey(Ops[CallIndex]), ProjectCall);
    Calls.emplace(*nativeSourceCallKey(Ops[ConsumeIndex]), ConsumeCall);
  }
};

TEST(NativeSourceHints, FrameOrExternalReturnBorrowsStayBounded) {
  for (unsigned Mutation = 0; Mutation < 17; ++Mutation) {
    SCOPED_TRACE(Mutation);
    NativeFrameAliasFixture F;
    auto &Ops = F.Low.Blocks[0].Ops;
    auto &Project = F.Calls.begin()->second;
    auto &Consume = F.Calls.rbegin()->second;
    switch (Mutation) {
    case 0:
      break;
    case 1:
      Consume.ReadOnlyFrameParameters.clear();
      break;
    case 2:
      Consume.ReadOnlyFrameParameters[0] = 25;
      break;
    case 3:
      Consume.WritableFrameParameters[0] = 8;
      break;
    case 4:
      Ops.insert(Ops.begin() + F.ConsumeIndex,
                 F.op(NdOp::STORE, {},
                      {NdVar::reg(a64reg::X2, 8), NdVar::reg(a64reg::X0, 8)},
                      0x1014));
      break;
    case 5:
      Ops.insert(Ops.begin() + F.ConsumeIndex,
                 F.op(NdOp::COPY, NdVar::reg(a64reg::SP, 8),
                      {NdVar::reg(a64reg::X0, 8)}, 0x1014));
      break;
    case 6:
      Ops.insert(Ops.begin() + F.ConsumeIndex,
                 F.op(NdOp::INT_ADD, NdVar::reg(a64reg::X0, 8),
                      {NdVar::reg(a64reg::X0, 8), NdVar::cst(1, 8)}, 0x1014));
      break;
    case 7:
      Ops.insert(Ops.begin() + F.ConsumeIndex,
                 F.op(NdOp::COPY, NdVar::reg(a64reg::X0 + 1, 1),
                      {NdVar::cst(0, 1)}, 0x1014));
      break;
    case 8:
      Project.ReturnFrameOrExternal->Parameter = 1;
      break;
    case 9:
      Project.ReturnFrameOrExternal->Bytes = 25;
      break;
    case 10:
      Project.ReturnFrameOrExternal->Bytes = 0;
      break;
    case 11:
      F.Project.ReturnLocation.ValueBytes = 4;
      F.Project.ReturnType = NdType::makeInt(4);
      break;
    case 12:
      Project.WritableFrameParameters.clear();
      break;
    case 13:
      Project.Termination =
          NativeSourceCallContract::TerminationKind::StackCheckFailure;
      break;
    case 14:
      Ops.insert(Ops.begin() + F.ConsumeIndex,
                 F.op(NdOp::INT_ADD, NdVar::reg(a64reg::SP, 8),
                      {NdVar::reg(a64reg::SP, 8), NdVar::cst(32, 8)}, 0x1014));
      break;
    case 15:
      // It may alias the frame, so an indirect call through that result has
      // no callable target proof even when the scalar signature looks valid.
      Ops[F.ConsumeIndex].Opcode = NdOp::INDIR_CALL;
      Ops[F.ConsumeIndex].Inputs[0] = NdVar::reg(a64reg::X0, 8);
      {
        auto Contract = Consume;
        F.Calls.erase(std::prev(F.Calls.end()));
        F.Calls.emplace(*nativeSourceCallKey(Ops[F.ConsumeIndex]), Contract);
      }
      break;
    case 16:
      Consume.ReadOnlyFrameParameters[1] = 8;
      break;
    }
    EXPECT_EQ(restoresNativeSourceState(F.Low, Arch::AArch64, F.Calls),
              Mutation == 0);
  }
}

TEST(NativeSourceHints,
     FrameOrExternalReturnRetainsTaintThroughCopiesAndJoins) {
  for (bool Writable : {false, true}) {
    NativeFrameAliasFixture F;
    auto &Ops = F.Low.Blocks[0].Ops;
    Ops.insert(Ops.begin() + F.ConsumeIndex,
               F.op(NdOp::COPY, NdVar::reg(a64reg::X9, 8),
                    {NdVar::reg(a64reg::X0, 8)}, 0x1014));
    Ops.insert(Ops.begin() + F.ConsumeIndex + 1,
               F.op(NdOp::COPY, NdVar::reg(a64reg::X0, 8),
                    {NdVar::reg(a64reg::X9, 8)}, 0x1014));
    auto &Consume = F.Calls.rbegin()->second;
    if (Writable) {
      Consume.ReadOnlyFrameParameters.clear();
      Consume.WritableFrameParameters.emplace(0, 24);
    }
    EXPECT_TRUE(restoresNativeSourceState(F.Low, Arch::AArch64, F.Calls));
    // A result carried through both reaching paths must keep its possible
    // frame provenance. Removing the consumer's borrow must still fail.
    auto Suffix =
        std::vector<LowOp>(Ops.begin() + F.ConsumeIndex + 2, Ops.end());
    Ops.resize(F.ConsumeIndex + 2);
    F.Low.Blocks[0].Succs = {1, 2};
    for (int I : {1, 2}) {
      LowBlock B;
      B.Id = I;
      B.Preds = {0};
      B.Succs = {3};
      B.Ops.push_back(F.op(NdOp::NOP, {}, {}, 0x1014));
      F.Low.Blocks.push_back(std::move(B));
    }
    LowBlock Merge;
    Merge.Id = 3;
    Merge.Preds = {1, 2};
    Merge.Ops = std::move(Suffix);
    F.Low.Blocks.push_back(std::move(Merge));
    EXPECT_TRUE(restoresNativeSourceState(F.Low, Arch::AArch64, F.Calls));
    Consume.ReadOnlyFrameParameters.clear();
    Consume.WritableFrameParameters.clear();
    EXPECT_FALSE(restoresNativeSourceState(F.Low, Arch::AArch64, F.Calls));
  }
}

TEST(NativeSourceHints,
     FrameOrExternalReturnCannotEscapeOrRestoreSpillIdentity) {
  {
    NativeFrameAliasFixture F;
    auto &Ops = F.Low.Blocks[0].Ops;
    Ops.erase(Ops.begin() + F.ConsumeIndex);
    F.Calls.erase(std::prev(F.Calls.end()));
    // Restoring SP and the callee-saved registers does not make a returned
    // private-frame address valid after this invocation ends.
    EXPECT_FALSE(restoresNativeSourceState(F.Low, Arch::AArch64, F.Calls,
                                           nullptr, &F.Project));
    Ops.insert(std::prev(Ops.end()), F.op(NdOp::COPY, NdVar::reg(a64reg::X0, 8),
                                          {NdVar::cst(42, 8)}, 0x1020));
    EXPECT_TRUE(restoresNativeSourceState(F.Low, Arch::AArch64, F.Calls,
                                          nullptr, &F.Project));
  }
  {
    NativeFrameAliasFixture F;
    auto &Ops = F.Low.Blocks[0].Ops;
    auto &Project = F.Calls.begin()->second;
    Project.WritableFrameParameters.clear();
    Project.ReadOnlyFrameParameters.emplace(0, 24);
    Ops[F.CallIndex - 1] =
        F.op(NdOp::INT_ADD, NdVar::reg(a64reg::X0, 8),
             {NdVar::reg(a64reg::SP, 8), NdVar::cst(32, 8)}, 0x100c);
    Ops.erase(Ops.begin() + F.ConsumeIndex);
    F.Calls.erase(std::prev(F.Calls.end()));
    EXPECT_TRUE(restoresNativeSourceState(F.Low, Arch::AArch64, F.Calls));
    for (auto &Op : Ops)
      if (Op.Opcode == NdOp::LOAD && Op.Output == NdVar::reg(a64reg::X19, 8))
        Op.Inputs[0] = NdVar::reg(a64reg::X0, 8);
    // The result might be external. It must not read the saved x19 identity
    // merely because one of its possible addresses equals that spill slot.
    EXPECT_FALSE(restoresNativeSourceState(F.Low, Arch::AArch64, F.Calls));
  }
}

TEST(NativeSourceHints, ReturningCallsConsumeCompletelyWrittenStackWords) {
  for (unsigned Mutation = 0; Mutation < 13; ++Mutation) {
    SCOPED_TRACE(Mutation);
    NativeOutgoingStackFixture F(Mutation == 10 ? Arch::X64 : Arch::AArch64);
    const auto &TRI = getTargetRegInfo(F.Architecture);
    auto &Ops = F.Low.Blocks[0].Ops;
    if (Mutation == 1)
      Ops[F.ArgumentStore].Inputs[1].Size = 4;
    if (Mutation == 2)
      Ops[F.ArgumentStore] = NativeVoidFrameFixture::op(NdOp::NOP, {}, {});
    if (Mutation == 3)
      Ops[F.ArgumentStore].Inputs[1] = NdVar::reg(TRI.StackPointer, 8);
    if (Mutation == 4) {
      // The existing spill is fully written and contains a valid entry value.
      // Passing it by value lets the callee overwrite it, so that slot cannot
      // still restore the preserved register on return.
      F.Binding->Signature.Parameters.back().Location.EntryStackOffset = 16;
    }
    if (Mutation == 5)
      Ops[F.RestoreIndex + 1].Opcode = NdOp::NOP;
    if (Mutation == 6)
      F.Binding->Signature.Parameters.back().Location.EntryStackOffset =
          F.FrameBytes;
    if (Mutation == 7) {
      LowBlock CallBlock;
      CallBlock.Id = 1;
      CallBlock.Preds = {0};
      CallBlock.Ops.assign(Ops.begin() + F.CallIndex, Ops.end());
      Ops.resize(F.CallIndex);
      F.Low.Blocks[0].Succs = {1};
      F.Low.Blocks.push_back(std::move(CallBlock));
    }
    if (Mutation == 8 || Mutation == 9) {
      auto Second = F.Med.Blocks[0].Ops[0];
      Second.Addr += 4;
      ++Second.OriginSeq;
      F.Med.Blocks[0].Ops.insert(F.Med.Blocks[0].Ops.begin() + 1, Second);
      auto SecondLow = Ops[F.CallIndex];
      SecondLow.Addr = Second.Addr;
      SecondLow.Seq = Second.OriginSeq;
      Ops.insert(Ops.begin() + F.RestoreIndex, SecondLow);
      if (Mutation == 9) {
        auto Rewrite = Ops[F.ArgumentStore];
        Rewrite.Addr = Second.Addr;
        Ops.insert(Ops.begin() + F.RestoreIndex, Rewrite);
      }
    }
    if (Mutation == 10)
      // The x64 return address is below the caller's SP. This is a valid
      // x64 ABI declaration; only the ARM64 extension remains unavailable.
      F.Binding->Signature.Parameters.back().Location.EntryStackOffset = 8;
    if (Mutation == 11) {
      F.Binding->Signature.Parameters.back().Type = NdType::makeInt(4);
      F.Binding->Signature.Parameters.back().Location.ValueBytes = 4;
      F.Med.Blocks[0].Ops[0].Inputs[2].Size = 4;
    }
    if (Mutation == 12) {
      // The actual argument at SP+24 is separate from both saved values in
      // the preceding gap. The entire incoming argument area may be changed,
      // so preserving only the individual argument slot would be unsound.
      for (auto &Op : Ops)
        if (Op.Opcode == NdOp::INT_ADD && Op.Output == NdVar::tmp(TmpBase, 8) &&
            Op.Inputs[1].Offset == 24)
          Op.Inputs[1].Offset = 8;
      F.Binding->Signature.Parameters.back().Location.EntryStackOffset = 24;
      const auto Address = NdVar::tmp(TmpBase + 8, 8);
      Ops[F.ArgumentStore].Inputs[0] = Address;
      Ops.insert(Ops.begin() + F.ArgumentStore,
                 NativeVoidFrameFixture::op(
                     NdOp::INT_ADD, Address,
                     {NdVar::reg(TRI.StackPointer, 8), NdVar::cst(24, 8)},
                     0x100c));
    }
    std::string Error;
    ASSERT_TRUE(validateSourceABI(F.Binding->Signature, Error)) << Error;
    const auto Hint = F.inferVoid(Error);
    if (Mutation == 0 || Mutation == 9) {
      ASSERT_TRUE(Hint) << Error;
      EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
    } else {
      EXPECT_FALSE(Hint);
      EXPECT_FALSE(Error.empty());
    }
  }
}

TEST(NativeSourceHints, IndependentCallOnlyContractsRetainObservedContext) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
      SCOPED_TRACE(unsigned(Architecture));
      SCOPED_TRACE(Mutation);
      NativeVoidFrameFixture F(Architecture);
      const auto Context =
          Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R12;
      SourceFunctionTypeHint Signature;
      Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftRuntime;
      Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
      std::string Error;
      ASSERT_TRUE(assignDarwinScalarSourceABI(Signature, Architecture, Error));
      auto Binding = std::make_shared<SourceCallTypeHint>();
      Binding->CallKind = SourceCallTypeHint::Kind::Native;
      Binding->TargetAddress = 0x1080;
      Binding->Signature = Signature;
      auto &Call = F.Med.Blocks[0].Ops[0];
      Call.SourceCallHint = Binding;
      Call.NumInputs = 1;
      // Consume an unmodified preserved context after the call, while the
      // surrounding frame saves/restores other preserved registers.
      MedVar Entry;
      Entry.Kind = MedVar::Reg;
      Entry.Id = 200;
      Entry.RegOff = Context;
      Entry.Size = 8;
      Entry.TheArch = Architecture;
      MedOp Store;
      Store.Opcode = NdOp::STORE;
      Store.addInput(MedVar::makeConst(0x3000, 8));
      Store.addInput(Entry);
      F.Med.Blocks[0].Ops.insert(F.Med.Blocks[0].Ops.begin() + 1, Store);
      F.Low.Blocks[0].Ops.insert(
          F.Low.Blocks[0].Ops.begin() + F.RestoreIndex,
          NativeVoidFrameFixture::op(
              NdOp::STORE, {},
              {NdVar::cst(0x3000, 8), NdVar::reg(Context, 8)}));
      NativeSourceCalleeContracts Contracts{&F.Image, {{0x1080, Signature}}};
      BinaryImage OtherImage;
      if (Mutation == 1)
        Contracts.ZeroArgumentPointerCallees.clear();
      if (Mutation == 2)
        Contracts.SourceImage = &OtherImage;
      if (Mutation == 3) {
        Contracts.ZeroArgumentPointerCallees.clear();
        Contracts.ZeroArgumentPointerCallees.emplace(0x1090, Signature);
      }
      if (Mutation == 4)
        Contracts.ZeroArgumentPointerCallees[0x1080].ReturnType =
            NdType::makeInt(8);
      if (Mutation == 5)
        Binding->TargetAddress = 0x1090;
      if (Mutation == 6)
        Binding->Signature.ReturnLocation.RegisterOffset += 8;
      if (Mutation == 7)
        ++F.Low.Blocks[0].Ops[F.CallIndex].Seq;
      if (Mutation == 8)
        F.Low.Blocks[0].Ops[F.RestoreIndex + 2].Opcode = NdOp::NOP;
      if (Mutation == 9)
        Binding->DoesNotReturn = true;
      if (Mutation == 10) {
        Binding->Signature.Parameters.push_back(
            {"hidden", NdType::makePtr(NdType::makeVoid())});
        ASSERT_TRUE(assignDarwinScalarSourceABI(Binding->Signature,
                                                Architecture, Error));
        Contracts.ZeroArgumentPointerCallees[0x1080] = Binding->Signature;
      }
      const auto Hint = inferNativeSourceTypeHint(
          F.Image, F.Med, F.High, F.Audit, Error, &F.Low, false,
          Mutation == 11 ? nullptr : &Contracts);
      if (Mutation) {
        EXPECT_FALSE(Hint);
        EXPECT_FALSE(Error.empty());
      } else {
        ASSERT_TRUE(Hint) << Error;
        EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
        ASSERT_EQ(Hint->Parameters.size(), 2U);
        EXPECT_EQ(Hint->Parameters.back().Location.RegisterOffset, Context);
        EXPECT_EQ(Hint->Parameters.back().Location.ValueBytes, 8U);
      }
    }
}

TEST(NativeSourceHints,
     CanonicalDynamicValueWitnessDestroyRestoresFramedState) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeVoidFrameFixture Fixture(Architecture);
    Fixture.useValueWitnessDestroy();
    std::string Error;
    const auto Hint = Fixture.inferVoid(Error);
    ASSERT_TRUE(Hint) << Error;
    EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
  }
}

TEST(NativeSourceHints, VoidFramesRestoreEntryBytesAcrossBranchesAndLoops) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Shape = 0; Shape < 4; ++Shape) {
      NativeVoidFrameFixture Fixture(Architecture);
      if (Shape == 3)
        Fixture.usePreindexedSpills();
      else if (Shape)
        Fixture.splitReturns(Shape == 2);
      std::string Error;
      const auto Hint = Fixture.inferVoid(Error);
      ASSERT_TRUE(Hint) << unsigned(Architecture) << ": " << Shape << ": "
                        << Error;
      EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
      EXPECT_EQ(Hint->ReturnLocation.Kind, SourceABICarrierKind::None);
      EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                MedReturnValueEvidence::Unknown);
    }
}

TEST(NativeSourceHints, BoundObjCDispatchRequiresCompleteFramedCallEvidence) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (auto Kind : {SourceCallTypeHint::Kind::ObjCMessage,
                      SourceCallTypeHint::Kind::ObjCSuper2})
      for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
        SCOPED_TRACE(Mutation);
        NativeVoidFrameFixture F(Architecture);
        auto &Call = F.Med.Blocks[0].Ops[0];
        auto Hint = std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
        Hint->CallKind = Kind;
        Hint->Signature.Origin =
            SourceFunctionTypeHint::OriginKind::ObjCRuntime;
        Hint->Signature.Parameters.push_back(
            {"selector", NdType::makePtr(NdType::makeVoid())});
        std::string Error;
        ASSERT_TRUE(
            assignDarwinObjCSourceABI(Hint->Signature, Architecture, Error));
        Call.addInput(MedVar::makeConst(0x1030, 8));
        Call.SourceCallHint = Hint;
        if (Mutation == 1)
          Hint->Signature.Origin =
              SourceFunctionTypeHint::OriginKind::NativeAnalysis;
        if (Mutation == 2)
          Hint->Signature.HasExplicitABI = false;
        if (Mutation == 3)
          Hint->DoesNotReturn = true;
        if (Mutation == 4)
          ++Call.OriginSeq;
        EXPECT_EQ(bool(F.inferVoid(Error)), Mutation == 0) << Error;
      }
}

TEST(NativeSourceHints, IndirectResultsNeedAnExplicitFramePreservationProof) {
  for (bool Framed : {false, true}) {
    NativeVoidFixture Fixture =
        Framed ? NativeVoidFixture(NativeVoidFrameFixture(Arch::AArch64))
               : NativeVoidFixture(Arch::AArch64);
    std::string Error;
    ASSERT_TRUE(Fixture.inferVoid(Error)) << Error;
    auto &Call = Fixture.Med.Blocks[0].Ops[0];
    auto Hint = std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
    Hint->CallKind = SourceCallTypeHint::Kind::Native;
    Hint->TargetAddress = 0x1080;
    Hint->Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Hint->Signature.ReturnType = NdType::makeStruct(
        {NdType::makeInt(8), NdType::makeInt(8), NdType::makeInt(8)});
    ASSERT_TRUE(
        assignDarwinFixedSourceABI(Hint->Signature, Arch::AArch64, Error));
    Call.SourceCallHint = Hint;
    // The apparently unchanged explicit arguments hide a writable result
    // pointer into the frame. It may overwrite the saved register spills.
    auto &Ops = Fixture.Low.Blocks[0].Ops;
    const auto Where = std::find_if(Ops.begin(), Ops.end(), [](const auto &Op) {
      return Op.Opcode == NdOp::CALL;
    });
    ASSERT_NE(Where, Ops.end());
    const auto &TRI = getTargetRegInfo(Arch::AArch64);
    Ops.insert(Where, NativeVoidFrameFixture::op(
                          NdOp::COPY, NdVar::reg(TRI.indirectResultReg(), 8),
                          {NdVar::reg(TRI.StackPointer, 8)}, 0x100c));
    EXPECT_FALSE(Fixture.inferVoid(Error));
  }
}

TEST(NativeSourceHints,
     PreservedContextSurvivesBorrowedObjCSuperFrameArgument) {
  NativeVoidFrameFixture Fixture(Arch::AArch64);
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  const auto Context = a64reg::X19;

  auto CallHint = std::make_shared<SourceCallTypeHint>(
      *Fixture.Med.Blocks[0].Ops[0].SourceCallHint);
  CallHint->CallKind = SourceCallTypeHint::Kind::ObjCSuper2;
  CallHint->Signature.Origin = SourceFunctionTypeHint::OriginKind::ObjCRuntime;
  CallHint->Signature.Parameters.push_back(
      {"selector", NdType::makePtr(NdType::makeVoid())});
  std::string Error;
  ASSERT_TRUE(
      assignDarwinObjCSourceABI(CallHint->Signature, Arch::AArch64, Error));
  // A super message ABI alone does not lend private frame storage. Supply
  // the exact current runtime import and declaration used by the effect owner.
  Fixture.Image.MachOTwoLevelNamespace = true;
  Fixture.Image.DynInfo.NeededLibs = {
      "/usr/lib/libobjc.A.dylib",
      "/System/Library/Frameworks/QuartzCore.framework/QuartzCore"};
  Section TextSection;
  TextSection.VA = 0x1000;
  TextSection.Size = TextSection.FileSz = 0x100;
  TextSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  TextSection.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
  Fixture.Image.Sections.push_back(TextSection);
  Segment ImportData;
  ImportData.VA = 0x3000;
  ImportData.Size = ImportData.FileSz = 8;
  ImportData.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  ImportData.Data.resize(8);
  Fixture.Image.Segments.push_back(ImportData);
  Fixture.Image.ImportPtrSlots[0x3000] = "_objc_msgSendSuper2";
  Fixture.Image.DyldBindSlots[0x3000] = {"_objc_msgSendSuper2", 0,
                                         "/usr/lib/libobjc.A.dylib", false};
  const uint32_t Stub[] = {0xd0000010, 0xf9400210, 0xd61f0200};
  for (unsigned I = 0; I < std::size(Stub); ++I)
    llvm::support::endian::write32le(
        Fixture.Image.Segments[0].Data.data() + 0x80 + I * 4, Stub[I]);
  CallHint->TargetAddress = 0x1080;
  CallHint->TargetName = "objc_msgSendSuper2";
  CallHint->Selector = "layoutSublayers";
  const auto Declaration =
      objcSelectorSourceTypeHint(Fixture.Image, CallHint->Selector);
  ASSERT_TRUE(Declaration);
  CallHint->Signature = *Declaration;
  ASSERT_TRUE(objcSuperSourceFrameEffects(Fixture.Image, *CallHint));
  for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = Fixture.Image;
    auto Binding = *CallHint;
    switch (Mutation) {
    case 0:
      Image.DyldBindSlots.at(0x3000).WeakImport = true;
      break;
    case 1:
      Image.DyldBindSlots.at(0x3000).Addend = 8;
      break;
    case 2:
      Image.DyldBindSlots.at(0x3000).Module = "/other/libobjc";
      break;
    case 3:
      Image.ImportPtrSlots[0x3000] = "_objc_msgSend";
      break;
    case 4:
      Image.DynInfo.NeededLibs.clear();
      break;
    case 5:
      Image.Segments[0].Data[0x80] ^= 1;
      break;
    case 6:
      Binding.WeakImport = true;
      break;
    case 7:
      Binding.DoesNotReturn = true;
      break;
    case 8:
      Binding.TargetName = "objc_msgSend";
      break;
    case 9:
      Binding.Selector = "missingIndependentDeclaration";
      break;
    case 10:
      Binding.Signature.Parameters[0].Location.ValueBytes = 4;
      break;
    case 11:
      Binding.TargetAddress += 4;
      break;
    }
    EXPECT_FALSE(objcSuperSourceFrameEffects(Image, Binding));
  }
  auto &MedCall = Fixture.Med.Blocks[0].Ops[0];
  MedCall.SourceCallHint = CallHint;

  MedVar ContextValue;
  ContextValue.Kind = MedVar::Reg;
  ContextValue.Id = 200;
  ContextValue.RegOff = Context;
  ContextValue.Size = 8;
  ContextValue.TheArch = Arch::AArch64;
  MedCall.addInput(ContextValue);

  auto &Ops = Fixture.Low.Blocks[0].Ops;
  // Keep the saved context live until the call instead of using it as a
  // scratch value immediately after the spill.
  ASSERT_EQ(Ops[3].Output, NdVar::reg(Context, 8));
  Ops.erase(Ops.begin() + 3);
  --Fixture.CallIndex;
  --Fixture.RestoreIndex;
  using F = NativeVoidFrameFixture;
  const auto SP = NdVar::reg(TRI.StackPointer, 8);
  Ops.insert(Ops.begin() + Fixture.CallIndex,
             {F::op(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[0], 8), {SP}),
              F::op(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[1], 8),
                    {NdVar::reg(Context, 8)})});

  const auto Hint = Fixture.inferVoid(Error);
  ASSERT_TRUE(Hint) << Error;
  ASSERT_EQ(Hint->Parameters.size(), 2U);
  EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, Context);
  EXPECT_EQ(Hint->Parameters[1].Location.ValueBytes, 8U);

  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    auto Changed = Fixture;
    auto &ChangedOps = Changed.Low.Blocks[0].Ops;
    auto &FrameArgument = ChangedOps[Changed.CallIndex];
    if (Mutation == 0) {
      auto Other = std::make_shared<SourceCallTypeHint>(
          *Changed.Med.Blocks[0].Ops[0].SourceCallHint);
      Other->CallKind = SourceCallTypeHint::Kind::ObjCMessage;
      Changed.Med.Blocks[0].Ops[0].SourceCallHint = std::move(Other);
    } else if (Mutation == 1) {
      FrameArgument.Output.Size = FrameArgument.Inputs[0].Size = 4;
    } else if (Mutation < 4) {
      FrameArgument.Opcode = NdOp::INT_ADD;
      FrameArgument.addInput(NdVar::cst(Mutation == 2 ? 24 : uint64_t(-8), 8));
    } else
      ChangedOps[Changed.CallIndex + 1].Inputs[0] = NdVar::cst(0x1030, 8);
    const auto Rejected = Changed.inferVoid(Error);
    if (Rejected)
      EXPECT_EQ(Rejected->Parameters.size(), 1U) << Mutation;
    else
      EXPECT_FALSE(Error.empty()) << Mutation;
  }
}

TEST(NativeSourceHints, VoidFramesKeepBoundCallResultsInsideTheHelper) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      SCOPED_TRACE(Mutation);
      NativeVoidFrameFixture Fixture(Architecture);
      const auto &TRI = getTargetRegInfo(Architecture);
      auto Hint = std::make_shared<SourceCallTypeHint>(
          *Fixture.Med.Blocks[0].Ops[0].SourceCallHint);
      Hint->Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
      std::string Error;
      ASSERT_TRUE(
          assignDarwinScalarSourceABI(Hint->Signature, Architecture, Error));
      auto Call = Fixture.Med.Blocks[0].Ops[0];
      Call.Addr = 0x100c;
      Call.OriginSeq = 3;
      Call.SourceCallHint = Hint;
      Call.Output.Kind = MedVar::Reg;
      Call.Output.Id = 30;
      Call.Output.SSAVer = 1;
      Call.Output.TheArch = Architecture;
      Call.Output.RegOff = TRI.IntReturnReg;
      Call.Output.Size = 8;
      // The returned object is consumed by the following void release. It
      // cannot become a result of the enclosing helper after that call.
      Fixture.Med.Blocks[0].Ops[0].Inputs[1] = Call.Output;
      Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin(), Call);
      auto NativeCall = Fixture.Low.Blocks[0].Ops[Fixture.CallIndex];
      NativeCall.Addr = Call.Addr;
      NativeCall.Seq = Call.OriginSeq;
      Fixture.Low.Blocks[0].Ops.insert(
          Fixture.Low.Blocks[0].Ops.begin() + Fixture.CallIndex, NativeCall);
      if (Mutation == 1)
        Hint->Signature.ReturnLocation.ValueBytes = 4;
      if (Mutation == 2)
        Fixture.Med.Blocks[0].Ops[0].SourceCallHint.reset();
      if (Mutation == 3)
        ++Fixture.Low.Blocks[0].Ops[Fixture.CallIndex].Seq;
      if (Mutation == 4)
        Hint->DoesNotReturn = true;
      if (Mutation == 5) {
        // A typed result does not justify passing the private frame to a
        // callee or skipping the existing preservation/escape proof.
        Fixture.Low.Blocks[0].Ops.insert(
            Fixture.Low.Blocks[0].Ops.begin() + Fixture.CallIndex,
            NativeVoidFrameFixture::op(NdOp::COPY,
                                       NdVar::reg(TRI.IntParamRegs[0], 8),
                                       {NdVar::reg(TRI.StackPointer, 8)}));
      }
      const auto Result = Fixture.inferVoid(Error);
      if (Mutation) {
        EXPECT_FALSE(Result) << Error;
      } else {
        ASSERT_TRUE(Result) << Error;
        EXPECT_EQ(Result->ReturnType->Kind, NdTypeKind::Void);
        EXPECT_EQ(Result->ReturnLocation.Kind, SourceABICarrierKind::None);
        EXPECT_EQ(Result->ReturnLocation.ValueBytes, 0U);
      }
    }
}

TEST(NativeSourceHints, VoidFramesKeepSpillsAcrossDisjointExternalStores) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeVoidFrameFixture Fixture(Architecture);
    const auto &TRI = getTargetRegInfo(Architecture);
    Fixture.Low.Blocks[0].Ops.insert(
        Fixture.Low.Blocks[0].Ops.begin() + Fixture.CallIndex,
        NativeVoidFrameFixture::op(
            NdOp::STORE, {},
            {NdVar::reg(TRI.IntParamRegs[0], 8), NdVar::cst(0, 8)}));
    std::string Error;
    const auto Result = Fixture.inferVoid(Error);
    ASSERT_TRUE(Result) << unsigned(Architecture) << ": " << Error;
    EXPECT_EQ(Result->ReturnType->Kind, NdTypeKind::Void);
  }
}

TEST(NativeSourceHints,
     VoidFramesPreserveRestoredLowVectorBytesThroughNormalization) {
  NativeVoidFrameFixture Fixture(Arch::AArch64);
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  const uint64_t V8 = TRI.VecRegBase + 8 * TRI.VecRegStride;
  auto &Ops = Fixture.Low.Blocks[0].Ops;
  const auto Restore = std::find_if(
      Ops.begin() + Fixture.RestoreIndex, Ops.end(), [&](const LowOp &Op) {
        return Op.Opcode == NdOp::LOAD && Op.Output == NdVar::reg(V8, 8);
      });
  ASSERT_NE(Restore, Ops.end());
  const size_t Insert = std::distance(Ops.begin(), Restore) + 1;
  Ops.insert(Ops.begin() + Insert,
             {NativeVoidFrameFixture::op(NdOp::INT_ZEXT, NdVar::reg(V8, 16),
                                         {NdVar::reg(V8, 8)}, 0x1020),
              NativeVoidFrameFixture::op(NdOp::SUBBYTES, NdVar::reg(V8, 8),
                                         {NdVar::reg(V8, 16), NdVar::cst(0, 8)},
                                         0x1020),
              NativeVoidFrameFixture::op(NdOp::INT_ZEXT, NdVar::reg(V8, 16),
                                         {NdVar::reg(V8, 8)}, 0x1020)});

  std::string Error;
  ASSERT_TRUE(Fixture.inferVoid(Error)) << Error;

  auto Shifted = Fixture;
  Shifted.Low.Blocks[0].Ops[Insert + 1].Inputs[1] = NdVar::cst(1, 8);
  EXPECT_FALSE(Shifted.inferVoid(Error));

  auto Truncated = Fixture;
  Truncated.Low.Blocks[0].Ops[Insert].Inputs[0].Size = 4;
  EXPECT_FALSE(Truncated.inferVoid(Error));
}

TEST(NativeSourceHints,
     SwiftBeginAccessRejectsUnauthenticatedOriginalFrameCall) {
  NativeVoidFrameFixture Fixture(Arch::AArch64);
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  constexpr va_t ImportSlot = 0x2000;
  const std::string Import = "_swift_beginAccess";
  Segment Data;
  Data.VA = ImportSlot;
  Data.Size = Data.FileSz = 8;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(8);
  Fixture.Image.Segments.push_back(std::move(Data));
  Fixture.Image.ImportPtrSlots[ImportSlot] = Import;
  ASSERT_TRUE(Fixture.Image.recordDyldBindSlot(
      ImportSlot, Import, 0, "/usr/lib/swift/libswiftCore.dylib", false));
  const auto Runtime = swiftRuntimeSourceCallHint(Fixture.Image, ImportSlot);
  ASSERT_TRUE(Runtime);
  ASSERT_EQ(Runtime->Signature.Parameters.size(), 4U);

  auto &MedCall = Fixture.Med.Blocks[0].Ops[0];
  MedCall.SourceCallHint = std::make_shared<const SourceCallTypeHint>(*Runtime);
  MedCall.NumInputs = 1;
  MedCall.addInput(Fixture.Med.Params[0]);
  MedCall.addInput(MedVar::makeConst(0, 8));
  MedCall.addInput(MedVar::makeConst(1, 8));
  MedCall.addInput(MedVar::makeConst(0, 8));

  auto &Ops = Fixture.Low.Blocks[0].Ops;
  const auto SP = NdVar::reg(TRI.StackPointer, 8);
  ASSERT_EQ(Ops.front().Opcode, NdOp::INT_SUB);
  Ops.front().Inputs[1] = NdVar::cst(96, 8);
  ASSERT_EQ(Ops[Fixture.RestoreIndex + Fixture.Saved.size() * 2].Opcode,
            NdOp::INT_ADD);
  Ops[Fixture.RestoreIndex + Fixture.Saved.size() * 2].Inputs[1] =
      NdVar::cst(96, 8);
  Fixture.FrameBytes = 96;
  Ops.insert(Ops.begin() + Fixture.CallIndex,
             NativeVoidFrameFixture::op(NdOp::INT_ADD,
                                        NdVar::reg(TRI.IntParamRegs[1], 8),
                                        {SP, NdVar::cst(32, 8)}));
  ++Fixture.CallIndex;
  ++Fixture.RestoreIndex;

  std::string Error;
  const auto Hint = Fixture.inferVoid(Error);
  EXPECT_FALSE(Hint);

  auto Overlap = Fixture;
  Overlap.Low.Blocks[0].Ops[Overlap.CallIndex - 1].Inputs[1] = NdVar::cst(0, 8);
  EXPECT_FALSE(Overlap.inferVoid(Error));

  auto Forged = Fixture;
  auto ForgedBinding = std::make_shared<SourceCallTypeHint>(
      *Forged.Med.Blocks[0].Ops[0].SourceCallHint);
  ForgedBinding->TargetName += "_forged";
  Forged.Med.Blocks[0].Ops[0].SourceCallHint = std::move(ForgedBinding);
  EXPECT_FALSE(Forged.inferVoid(Error));
}

TEST(NativeSourceHints, SwiftEndAccessRejectsUninitializedPrivateFrameScratch) {
  NativeVoidFrameFixture Fixture(Arch::AArch64);
  constexpr va_t ImportSlot = 0x2000;
  constexpr llvm::StringLiteral Import = "_swift_endAccess";
  Segment Data;
  Data.VA = ImportSlot;
  Data.Size = Data.FileSz = 8;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(8);
  Fixture.Image.Segments.push_back(std::move(Data));
  Fixture.Image.ImportPtrSlots[ImportSlot] = Import.str();
  ASSERT_TRUE(Fixture.Image.recordDyldBindSlot(
      ImportSlot, Import.str(), 0, "/usr/lib/swift/libswiftCore.dylib", false));
  const auto Runtime = swiftRuntimeSourceCallHint(Fixture.Image, ImportSlot);
  ASSERT_TRUE(Runtime);
  ASSERT_EQ(Runtime->Signature.Parameters.size(), 1U);
  Fixture.Med.Blocks[0].Ops[0].SourceCallHint =
      std::make_shared<const SourceCallTypeHint>(*Runtime);

  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  auto &Ops = Fixture.Low.Blocks[0].Ops;
  const auto SP = NdVar::reg(TRI.StackPointer, 8);
  Ops.front().Inputs[1] = NdVar::cst(96, 8);
  Ops[Fixture.RestoreIndex + Fixture.Saved.size() * 2].Inputs[1] =
      NdVar::cst(96, 8);
  Fixture.FrameBytes = 96;
  Ops.insert(Ops.begin() + Fixture.CallIndex,
             NativeVoidFrameFixture::op(NdOp::INT_ADD,
                                        NdVar::reg(TRI.IntParamRegs[0], 8),
                                        {SP, NdVar::cst(32, 8)}));
  ++Fixture.CallIndex;

  std::string Error;
  const auto Hint = Fixture.inferVoid(Error);
  // An uninitialized record can carry a tracked-access link. endAccess may
  // then unlink it through a different record; a 24-byte borrow is
  // insufficient.
  EXPECT_FALSE(Hint);

  auto Overlap = Fixture;
  Overlap.Low.Blocks[0].Ops[Overlap.CallIndex - 1].Inputs[1] = NdVar::cst(0, 8);
  EXPECT_FALSE(Overlap.inferVoid(Error));

  auto Forged = Fixture;
  auto ForgedBinding = std::make_shared<SourceCallTypeHint>(
      *Forged.Med.Blocks[0].Ops[0].SourceCallHint);
  ForgedBinding->TargetName += "_forged";
  Forged.Med.Blocks[0].Ops[0].SourceCallHint = std::move(ForgedBinding);
  EXPECT_FALSE(Forged.inferVoid(Error));
}

TEST(NativeSourceHints, SwiftHasherBorrowsOnlyItsBoundedPrivateFrame) {
  for (const auto &Import :
       {"_$ss6HasherV5_seedABSi_tcfC", "_$sSS4hash4intoys6HasherVz_tF"}) {
    SCOPED_TRACE(Import);
    NativeVoidFrameFixture Fixture(Arch::AArch64);
    constexpr va_t ImportSlot = 0x2000;
    Segment Data;
    Data.VA = ImportSlot;
    Data.Size = Data.FileSz = 8;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(8);
    Fixture.Image.Segments.push_back(std::move(Data));
    Fixture.Image.ImportPtrSlots[ImportSlot] = Import;
    ASSERT_TRUE(Fixture.Image.recordDyldBindSlot(
        ImportSlot, Import, 0, "/usr/lib/swift/libswiftCore.dylib", false));
    const auto Runtime = swiftRuntimeSourceCallHint(Fixture.Image, ImportSlot);
    ASSERT_TRUE(Runtime);
    ASSERT_FALSE(Runtime->Signature.Parameters.empty());
    const auto BufferReg =
        Runtime->Signature.Parameters[0].Location.RegisterOffset;
    ASSERT_EQ(BufferReg, Import == std::string("_$ss6HasherV5_seedABSi_tcfC")
                             ? a64reg::X8
                             : a64reg::X0);

    auto &Ops = Fixture.Low.Blocks[0].Ops;
    const auto SP = NdVar::reg(a64reg::SP, 8);
    Ops.front().Inputs[1] = NdVar::cst(112, 8);
    Ops[Fixture.RestoreIndex + Fixture.Saved.size() * 2].Inputs[1] =
        NdVar::cst(112, 8);
    Ops.insert(Ops.begin() + Fixture.CallIndex,
               NativeVoidFrameFixture::op(NdOp::INT_ADD,
                                          NdVar::reg(BufferReg, 8),
                                          {SP, NdVar::cst(32, 8)}));
    ++Fixture.CallIndex;
    const auto Key = nativeSourceCallKey(Ops[Fixture.CallIndex]);
    ASSERT_TRUE(Key);
    NativeSourceCallContract Contract;
    Contract.Signature = &Runtime->Signature;
    Contract.WritableFrameParameters.emplace(0, 72);
    NativeSourceCalls Calls{{*Key, Contract}};
    EXPECT_TRUE(restoresNativeSourceState(Fixture.Low, Arch::AArch64, Calls));

    auto Overlap = Fixture;
    Overlap.Low.Blocks[0].Ops[Fixture.CallIndex - 1].Inputs[1] =
        NdVar::cst(24, 8);
    EXPECT_FALSE(restoresNativeSourceState(Overlap.Low, Arch::AArch64, Calls));

    Calls.at(*Key).WritableFrameParameters.clear();
    EXPECT_FALSE(restoresNativeSourceState(Fixture.Low, Arch::AArch64, Calls));
  }
}

TEST(NativeSourceHints, VoidFramesRejectClobbersEscapesAndStaleSpills) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 19; ++Mutation) {
      NativeVoidFrameFixture Fixture(Architecture);
      auto &Ops = Fixture.Low.Blocks[0].Ops;
      const auto &TRI = getTargetRegInfo(Architecture);
      const auto SP = NdVar::reg(TRI.StackPointer, 8);
      const auto Word = NdVar::reg(Fixture.Saved[0], 8);
      using F = NativeVoidFrameFixture;
      switch (Mutation) {
      case 0:
        Ops[Fixture.RestoreIndex + 1].Output.Size = 4;
        break;
      case 1:
        Ops.insert(Ops.begin() + Fixture.CallIndex,
                   F::op(NdOp::STORE, {}, {SP, NdVar::cst(0, 1)}));
        break;
      case 2: {
        // A partially copied stack address remains a possible private-frame
        // alias even though it is not a complete, exact frame address.
        Ops.insert(
            Ops.begin() + Fixture.CallIndex,
            {F::op(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[0], 4),
                   {NdVar::reg(TRI.StackPointer, 4)}),
             F::op(NdOp::STORE, {},
                   {NdVar::reg(TRI.IntParamRegs[0], 8), NdVar::cst(0, 8)})});
        break;
      }
      case 3:
        Ops.insert(Ops.begin() + Fixture.CallIndex,
                   F::op(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[0], 8), {SP}));
        break;
      case 4:
        Ops.insert(Ops.begin() + Fixture.CallIndex,
                   F::op(NdOp::STORE, {}, {SP, SP}));
        break;
      case 5:
        Ops[Ops.size() - 2].Inputs[1].Offset -= 8;
        break;
      case 6:
        Ops[Fixture.RestoreIndex + 1] =
            F::op(NdOp::COPY, Word, {NdVar::reg(TRI.IntReturnReg, 8)}, 0x1020);
        break;
      case 7:
        Ops[1].Inputs[1].Offset = Fixture.FrameBytes;
        break;
      case 8:
        Ops[2].MemoryOrdering = NdMemoryOrdering::Acquire;
        break;
      case 9:
        // The spill address is an instruction-local temporary, not a value
        // available merely because a later instruction reuses its number.
        Ops[2].Addr += 4;
        break;
      case 10: {
        const int64_t Exposed =
            Fixture.FrameBytes - (Architecture == Arch::X64 ? 8 : 0);
        Ops.insert(Ops.begin() + Fixture.CallIndex,
                   F::op(NdOp::INT_ADD, SP, {SP, NdVar::cst(Exposed, 8)}));
        Ops.insert(
            Ops.begin() + Fixture.RestoreIndex + 1,
            F::op(NdOp::INT_SUB, SP, {SP, NdVar::cst(Exposed, 8)}, 0x1020));
        break;
      }
      case 11:
        Fixture.splitReturns();
        Fixture.Low.Blocks[2].Ops[1].Output.Size = 4;
        break;
      case 12:
        Fixture.splitReturns();
        Fixture.Low.Blocks[2].Preds.clear();
        break;
      case 13:
        Fixture.splitReturns(true);
        Fixture.Low.Blocks[1].Ops = {
            F::op(NdOp::INT_SUB, SP, {SP, NdVar::cst(8, 8)})};
        break;
      case 14:
        if (Architecture == Arch::AArch64)
          Ops.back().Inputs[0] = NdVar::reg(TRI.IntReturnReg, 8);
        else
          Ops.insert(Ops.end() - 1,
                     F::op(NdOp::STORE, {}, {SP, NdVar::cst(0, 8)}, 0x1020));
        break;
      case 15:
        // A four-byte constant is zero-extended by the eight-byte ADD;
        // interpreting its high bit as a signed frame delta invents a spill.
        Ops[0].Opcode = NdOp::INT_ADD;
        Ops[0].Inputs[1] = NdVar::cst(uint32_t(-Fixture.FrameBytes), 4);
        break;
      case 16:
        Ops.insert(
            Ops.begin() + Fixture.CallIndex,
            F::op(NdOp::INDIR_BR, {}, {NdVar::reg(TRI.IntParamRegs[0], 8)}));
        break;
      case 17:
        // A stack address below the current SP is not allocated storage.
        Ops[1].Inputs[1] = NdVar::cst(uint64_t(-8), 8);
        break;
      case 18: {
        // A later instruction cannot retroactively allocate a stack slot.
        const auto StackWrite = Fixture.usePreindexedSpills();
        Ops[StackWrite].Addr += 4;
        break;
      }
      }
      std::string Error;
      EXPECT_FALSE(Fixture.inferVoid(Error))
          << unsigned(Architecture) << ": " << Mutation << ": " << Error;
    }
}

TEST(NativeSourceHints, VoidFrameByteProofHonorsImplicitZeroExtensions) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeVoidFrameFixture Fixture(Architecture);
    auto &Ops = Fixture.Low.Blocks[0].Ops;
    // Leave the original upper bytes untouched before loading only the low
    // word. That load implicitly clears the upper word on both architectures.
    Ops[3] = NativeVoidFrameFixture::op(NdOp::NOP, {}, {});
    Ops[Fixture.RestoreIndex + 1].Output.Size = 4;
    std::string Error;
    EXPECT_FALSE(Fixture.inferVoid(Error));
  }
}

std::pair<HighFunc, PipelineFunctionAudit>
nativeVoidInputCandidate(Arch Architecture) {
  const auto &TRI = getTargetRegInfo(Architecture);
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Hint.Architecture = Architecture;
  Hint.HasExplicitABI = true;
  Hint.ReturnType = NdType::makeVoid();
  const auto Auxiliary =
      Architecture == Arch::AArch64 ? a64reg::X8 : x86reg::RAX;
  Hint.Parameters = {
      {"ordinary",
       NdType::makeInt(8),
       {SourceABICarrierKind::IntegerRegister, TRI.IntParamRegs[0], 0, 8}},
      {"auxiliary",
       NdType::makeInt(8),
       {SourceABICarrierKind::IntegerRegister, Auxiliary, 0, 8}}};
  HighFunc Function;
  Function.Entry = 0x1000;
  Function.SourceTypeHint = Hint;
  Function.ReturnType = Hint.ReturnType;
  Function.Params = {{"ordinary", NdType::makeInt(8)},
                     {"auxiliary", NdType::makeInt(8)}};
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Function.Body.push_back(Return);
  PipelineFunctionAudit Audit;
  Audit.Entry = Function.Entry;
  Audit.Disposition = PipelineFunctionDisposition::Accepted;
  Audit.HasLowIR = Audit.HasMedIR = Audit.MedIRVerified = true;
  Audit.DecodedInstructions = Audit.LiftedInstructions = 1;
  return {std::move(Function), std::move(Audit)};
}

TEST(NativeSourceHints, RefinesOnlyUnusedAuxiliaryVoidSourceInputs) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto [Function, Audit] = nativeVoidInputCandidate(Architecture);
    const auto Refined = refineNativeSourceTypeHint(Function, Audit);
    ASSERT_TRUE(Refined);
    ASSERT_EQ(Refined->Parameters.size(), 1U);
    EXPECT_EQ(Refined->Parameters[0].Name, "ordinary");
    EXPECT_EQ(Refined->Parameters[0].Location.RegisterOffset,
              getTargetRegInfo(Architecture).IntParamRegs[0]);
    EXPECT_EQ(Function.SourceTypeHint->Parameters.size(), 2U);
    std::string Error;
    EXPECT_TRUE(validateSourceABI(*Refined, Error)) << Error;
  }
}

TEST(NativeSourceHints, ScalarInputRefinementPreservesReturnEvidence) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto [Function, Audit] = nativeVoidInputCandidate(Architecture);
      auto &Hint = *Function.SourceTypeHint;
      Hint.ReturnType = Function.ReturnType = NdType::makeInt(8);
      Hint.ReturnLocation = {SourceABICarrierKind::IntegerRegister,
                             getTargetRegInfo(Architecture).IntReturnReg, 0, 8};
      auto &Value = Function.Body.front().RetVal;
      Value = HighExpr::makeConst(19, 8);
      if (Mutation == 1) {
        MedVar Input;
        Input.Kind = MedVar::Param;
        Input.Id = 1;
        Input.Size = 8;
        Value = HighExpr::makeVar(Input, NdType::makeInt(8));
      } else if (Mutation == 2) {
        Value.reset();
      } else if (Mutation == 3) {
        Value = HighExpr::makeUndef(8);
      } else if (Mutation == 4) {
        Value = HighExpr::makeConst(19, 4);
      }
      const auto Refined = refineNativeSourceTypeHint(Function, Audit);
      if (Mutation) {
        EXPECT_FALSE(Refined);
        continue;
      }
      ASSERT_TRUE(Refined);
      ASSERT_EQ(Refined->Parameters.size(), 1U);
      EXPECT_EQ(Refined->Parameters[0].Name, "ordinary");
      EXPECT_TRUE(equalSourceTypes(Refined->ReturnType, Hint.ReturnType));
      EXPECT_EQ(Refined->ReturnLocation.RegisterOffset,
                Hint.ReturnLocation.RegisterOffset);
    }
}

TEST(NativeSourceHints, DefinedLocalsDoNotRetainUnusedPhysicalInputRegisters) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto [Function, Audit] = nativeVoidInputCandidate(Architecture);
      auto &Hint = *Function.SourceTypeHint;
      Hint.ReturnType = Function.ReturnType = NdType::makeInt(8);
      Hint.ReturnLocation = {SourceABICarrierKind::IntegerRegister,
                             getTargetRegInfo(Architecture).IntReturnReg, 0, 8};
      MedVar Local;
      Local.Kind = MedVar::Reg;
      Local.Id = 50001;
      Local.RegOff = Hint.Parameters[1].Location.RegisterOffset;
      Local.Size = 8;
      Local.TheArch = Architecture;
      Function.Body.front().RetVal =
          HighExpr::makeVar(Local, NdType::makeInt(8));
      HighStmt Assign;
      Assign.Kind = StmtKind::Assign;
      auto Dst = Local;
      if (Mutation == 4)
        Dst.Size = 4;
      Assign.Dst = HighExpr::makeVar(Dst, NdType::makeInt(Dst.Size));
      Assign.Val = HighExpr::makeConst(19, Dst.Size);
      if (Mutation == 2 || Mutation == 3) {
        HighStmt Branch;
        Branch.Kind = StmtKind::If;
        auto Input = Local;
        Input.Kind = MedVar::Param;
        Input.Id = 0;
        Input.RegOff = Hint.Parameters[0].Location.RegisterOffset;
        Branch.Cond = HighExpr::makeVar(Input, NdType::makeInt(8));
        Branch.Body = {Assign};
        if (Mutation == 3)
          Branch.ElseBody = {Assign};
        Function.Body.insert(Function.Body.begin(), Branch);
      } else if (Mutation != 1) {
        Function.Body.insert(Function.Body.begin(), Assign);
      }
      const auto Refined = refineNativeSourceTypeHint(Function, Audit);
      EXPECT_EQ(bool(Refined), Mutation == 0);
      if (Refined)
        EXPECT_EQ(Refined->Parameters.size(), 1U);
    }
}

TEST(NativeSourceHints, SourceInputRefinementRetainsUsesAndIncompleteBodies) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 23; ++Mutation) {
      auto [Function, Audit] = nativeVoidInputCandidate(Architecture);
      MedVar Input;
      Input.Kind = MedVar::Param;
      Input.Id = 1;
      Input.Size = 8;
      Input.TheArch = Architecture;
      Input.RegOff =
          Function.SourceTypeHint->Parameters[1].Location.RegisterOffset;
      auto Use = HighExpr::makeVar(Input, NdType::makeInt(8));
      HighStmt Statement;
      Statement.Kind = StmtKind::ExprStmt;
      Statement.Val = Use;
      switch (Mutation) {
      case 0:
        Function.Body.insert(Function.Body.begin(), Statement);
        break;
      case 1:
        Statement.Kind = StmtKind::Assign;
        Statement.Dst = Use;
        Statement.Val = HighExpr::makeConst(0, 8);
        Function.Body.insert(Function.Body.begin(), Statement);
        break;
      case 2:
        Function.Body[0].DefaultBody.push_back(Statement);
        break;
      case 3:
        Function.Body[0].EHClauseBodies.push_back({Statement});
        break;
      case 4:
        Function.Body[0].Cond = HighExpr::makeConst(0, 8);
        Function.Body[0].Cond->IntrinsicOutputs.push_back(Input);
        break;
      case 5:
        Input.Kind = MedVar::Reg;
        Input.SSAVer = 2;
        Input.RegOff += 4;
        Input.Size = 4;
        Function.Body[0].Cond = HighExpr::makeVar(Input, NdType::makeInt(4));
        break;
      case 6:
        Input.Id = 4;
        Function.Body[0].Cond = HighExpr::makeVar(Input, NdType::makeInt(8));
        break;
      case 7:
        Function.Params[1].Type = NdType::makeInt(4);
        break;
      case 8:
        Function.SourceTypeHint->Origin =
            SourceFunctionTypeHint::OriginKind::ObjCSDK;
        break;
      case 9:
        Function.Body.clear();
        break;
      case 10:
        Function.StructuredExceptionRegions = 1;
        break;
      case 11:
        Function.Body[0].Kind = StmtKind::Nop;
        break;
      case 12:
        Function.ReturnType = NdType::makeInt(8);
        break;
      case 13:
        Function.Body[0].Cond = HighExpr::makeConst(0, 8);
        Function.Body[0].Cond->Kind = ExprKind::Undef;
        break;
      case 14:
        Function.SourceTypeHint.reset();
        break;
      case 15: {
        auto Expression = HighExpr::makeConst(0, 8);
        for (unsigned I = 0; I < 130; ++I) {
          auto Outer = std::make_shared<HighExpr>();
          Outer->Kind = ExprKind::Cast;
          Outer->Type = NdType::makeInt(8);
          Outer->Operands = {Expression};
          Expression = Outer;
        }
        Function.Body[0].Cond = Expression;
        break;
      }
      case 16:
        Audit.Disposition = PipelineFunctionDisposition::Candidate;
        break;
      case 17:
        Audit.HasLowIR = false;
        break;
      case 18:
        Audit.DecodeFailures.push_back(0x1000);
        break;
      case 19:
        ++Audit.Entry;
        break;
      case 20:
        Audit.TruncatedPaths.push_back(0x1000);
        break;
      case 21:
        Function.Body[0].Cases.resize(65537);
        break;
      case 22:
        Function.Body[0].Cond = HighExpr::makeConst(0, 8);
        Function.Body[0].Cond->IntrinsicOutputs.resize(65537);
        break;
      }
      EXPECT_FALSE(refineNativeSourceTypeHint(Function, Audit)) << Mutation;
    }
}

struct NativeFloatingFixture : NativeFixture {
  NativeFloatingFixture(Arch Architecture, unsigned Width, bool Wide = false)
      : NativeFixture(Architecture) {
    const auto &TRI = getTargetRegInfo(Architecture);
    Med.ReturnType = High.ReturnType = NdType::makeFloat(Width);
    for (unsigned I = 0; I < 2; ++I) {
      Med.Params[I].RegOff = TRI.FPParamRegs[I];
      Med.Params[I].Size = Wide ? 16 : Width;
      Med.TypedParams[I].Type = High.Params[I].Type =
          NdType::makeInt(Med.Params[I].Size);
    }
    auto &Ops = Med.Blocks[0].Ops;
    auto Sum = Ops.front();
    auto Return = Ops.back();
    Sum.Opcode = NdOp::FLOAT_ADD;
    Sum.Output.RegOff = TRI.FPReturnReg;
    Sum.Output.Size = Width;
    Sum.Inputs[0] = Med.Params[0];
    Sum.Inputs[1] = Med.Params[1];
    Ops.clear();
    if (Wide) {
      for (unsigned I = 0; I < 2; ++I) {
        MedOp Extract;
        Extract.Opcode = NdOp::SUBBYTES;
        Extract.Output = Sum.Output;
        Extract.Output.Kind = MedVar::Temp;
        Extract.Output.Id = 20 + I;
        Extract.addInput(Med.Params[I]);
        Extract.addInput(MedVar::makeConst(0, 8));
        Ops.push_back(Extract);
        Sum.Inputs[I] = Extract.Output;
      }
      Sum.Output.Kind = MedVar::Temp;
      Ops.push_back(Sum);
      auto Upper = Ops.front();
      Upper.Output.Id = 22;
      Upper.Output.Size = 16 - Width;
      Upper.Inputs[1].ConstVal = Width;
      Ops.push_back(Upper);
      MedOp Join;
      Join.Opcode = NdOp::CONCAT;
      Join.Output = Sum.Output;
      Join.Output.Kind = MedVar::Reg;
      Join.Output.Id = 23;
      Join.Output.Size = 16;
      Join.addInput(Upper.Output);
      Join.addInput(Sum.Output);
      Ops.push_back(Join);
    } else {
      Ops.push_back(Sum);
    }
    Ops.push_back(Return);
  }
};

TEST(NativeSourceHints,
     FloatingPairDemandRequiresCompleteLowDoubleObservation) {
  for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    LowFunc F;
    F.Blocks.emplace_back();
    LowOp Call;
    Call.Opcode = NdOp::CALL;
    Call.addInput(NdVar::cst(0x1080, 8));
    LowOp Read;
    Read.Opcode = NdOp::COPY;
    Read.Output = NdVar::reg(a64reg::V(9), 16);
    Read.addInput(NdVar::reg(a64reg::V(1), Mutation == 1 ? 8 : 16));
    F.Blocks[0].Ops = {Call, Read};
    if (Mutation == 2)
      F.Blocks[0].Ops.back().Inputs[0].Size = 4;
    if (Mutation == 3)
      F.Blocks[0].Ops.back().Inputs[0].Offset += 8;
    if (Mutation == 4)
      F.Blocks[0].Ops.front().Opcode = NdOp::INDIR_CALL;
    if (Mutation >= 5 && Mutation <= 8) {
      LowOp Stop;
      Stop.Opcode = Mutation == 5   ? NdOp::INTRINSIC
                    : Mutation == 6 ? NdOp::INDIR_CALL
                                    : NdOp::COPY;
      if (Mutation >= 7) {
        Stop.Output = NdVar::reg(a64reg::V(1) + (Mutation == 8 ? 4 : 0), 4);
        Stop.addInput(NdVar::cst(0, 4));
      }
      F.Blocks[0].Ops.insert(F.Blocks[0].Ops.begin() + 1, Stop);
    }
    if (Mutation == 9) {
      F.Blocks[0].Ops.back().Opcode = NdOp::INT_XOR;
      F.Blocks[0].Ops.back().addInput(Read.Inputs[0]);
    }
    if (Mutation == 10) {
      F.Blocks[0].Ops.pop_back();
      F.Blocks.emplace_back();
      F.Blocks.back().Ops.push_back(Read);
    }
    if (Mutation == 11)
      F.Blocks[0].Ops.back().Inputs[0].Size = 32;
    EXPECT_EQ(observedNativeFloatingPairReturns(F, Arch::AArch64),
              Mutation <= 1 ? std::set<va_t>{0x1080} : std::set<va_t>{});
    EXPECT_TRUE(observedNativeFloatingPairReturns(F, Arch::X64).empty());
  }
}

TEST(NativeSourceHints, FloatingPairsRequireBothCurrentCompleteCarriers) {
  for (unsigned Mutation = 0; Mutation != 18; ++Mutation) {
    SCOPED_TRACE(Mutation);
    NativeFloatingFixture F(Arch::AArch64, 8);
    auto Second = F.Med.Blocks[0].Ops[0];
    Second.Opcode = NdOp::FLOAT_SUB;
    Second.Output.RegOff = a64reg::V(1);
    Second.Output.Id = 11;
    F.Med.Blocks[0].Ops.insert(F.Med.Blocks[0].Ops.end() - 1, Second);
    std::string Error;
    auto Scalar = F.infer(Error);
    ASSERT_TRUE(Scalar) << Error;
    F.Med.SourceTypeHint = F.High.SourceTypeHint = *Scalar;
    F.Med.SourceParametersBound = true;
    auto &Ops = F.Med.Blocks[0].Ops;
    switch (Mutation) {
    case 1:
      F.Med.SourceTypeHint->Origin = F.High.SourceTypeHint->Origin =
          SourceFunctionTypeHint::OriginKind::DarwinSDK;
      break;
    case 2:
      F.Audit.MedIRVerified = false;
      break;
    case 3:
      F.High.Entry += 4;
      break;
    case 4:
      F.Med.SourceParametersBound = false;
      break;
    case 5:
      F.High.SourceTypeHint->ReturnType = NdType::makeFloat(4);
      break;
    case 6:
      Ops[1].Output.Size = 4;
      break;
    case 7:
      Ops[0].Output.Size = 4;
      break;
    case 8:
      Ops[1].Output.RegOff += 8;
      break;
    case 9:
      Ops.erase(Ops.begin() + 1);
      break;
    case 10: {
      MedOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(MedVar::makeConst(0x1080, 8));
      Ops.insert(Ops.end() - 1, Call);
      break;
    }
    case 11:
      F.Med.Blocks[0].ExceptionalSuccs.emplace_back();
      break;
    case 12:
      F.Med.Blocks[0].Succs.push_back(99);
      break;
    case 13:
      F.Med.SourceTypeHint->Architecture = F.High.SourceTypeHint->Architecture =
          Arch::X64;
      break;
    case 14:
      F.Med.DoesNotReturn = true;
      break;
    case 15:
      F.Med.SourceTypeHint->ReturnLocation.ValueBytes =
          F.High.SourceTypeHint->ReturnLocation.ValueBytes = 4;
      break;
    case 16:
      F.Med.SourceTypeHint->ReturnType = F.High.SourceTypeHint->ReturnType =
          F.Med.ReturnType = F.High.ReturnType = NdType::makeInt(8);
      break;
    case 17:
      F.High.Body.clear();
      break;
    }
    const auto Pair =
        refineNativeFloatingPairReturnHint(F.Med, F.High, F.Audit);
    EXPECT_EQ(bool(Pair), Mutation == 0);
    if (Pair) {
      ASSERT_EQ(Pair->ReturnComponents.size(), 2U);
      EXPECT_EQ(Pair->ReturnType->Kind, NdTypeKind::Struct);
      for (unsigned I = 0; I != 2; ++I) {
        EXPECT_EQ(Pair->ReturnComponents[I].Kind,
                  SourceABICarrierKind::FloatingRegister);
        EXPECT_EQ(Pair->ReturnComponents[I].RegisterOffset, a64reg::V(I));
        EXPECT_EQ(Pair->ReturnComponents[I].ValueBytes, 8U);
      }
      EXPECT_TRUE(equalSourceABIs(*F.Med.SourceTypeHint, *Scalar));
      F.Med.SourceTypeHint = F.High.SourceTypeHint = *Pair;
      F.Med.ReturnType = F.High.ReturnType = Pair->ReturnType;
      EXPECT_FALSE(refineNativeFloatingPairReturnHint(F.Med, F.High, F.Audit));
    }
  }
}

TEST(NativeSourceHints,
     FloatingPairPathsMeetBothDoublesAcrossJoinsAndBackedges) {
  for (bool Loop : {false, true})
    for (bool Missing : {false, true}) {
      NativeFloatingFixture F(Arch::AArch64, 8);
      std::string Error;
      const auto Scalar = F.infer(Error);
      ASSERT_TRUE(Scalar) << Error;
      F.Med.SourceTypeHint = F.High.SourceTypeHint = *Scalar;
      F.Med.SourceParametersBound = true;
      const auto First = F.Med.Blocks[0].Ops[0];
      auto Second = First;
      Second.Output.RegOff = a64reg::V(1);
      Second.Output.Id = 11;
      const auto Return = F.Med.Blocks[0].Ops.back();
      F.Med.Blocks[0].Ops = {First};
      F.Med.Blocks[0].Succs = {1, 2};
      F.Med.Blocks.resize(4);
      for (int I = 1; I != 4; ++I)
        F.Med.Blocks[I].Id = I;
      F.Med.Blocks[1].Preds =
          Loop ? std::vector<int>{0, 1} : std::vector<int>{0};
      F.Med.Blocks[1].Succs =
          Loop ? std::vector<int>{1, 3} : std::vector<int>{3};
      F.Med.Blocks[1].Ops = {Second};
      F.Med.Blocks[2].Preds = {0};
      F.Med.Blocks[2].Succs = {3};
      Second.Output.Id = 12;
      F.Med.Blocks[2].Ops =
          Missing ? std::vector<MedOp>{} : std::vector<MedOp>{Second};
      F.Med.Blocks[3].Preds = {1, 2};
      F.Med.Blocks[3].Ops = {Return};
      EXPECT_EQ(
          bool(refineNativeFloatingPairReturnHint(F.Med, F.High, F.Audit)),
          !Missing)
          << "loop=" << Loop << " missing=" << Missing;
    }
}

TEST(NativeSourceHints, FloatingPairReliftingPublishesBothComputedDoubles) {
  NativeFixture F;
  F.Audit.DecodedInstructions = F.Audit.LiftedInstructions = 5;
  SourceFunctionTypeHint Scalar;
  Scalar.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Scalar.ReturnType = NdType::makeFloat(8);
  Scalar.Parameters = {{"a", NdType::makeFloat(8)},
                       {"b", NdType::makeFloat(8)}};
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(Scalar, Arch::AArch64, Error))
      << Error;
  LowFunc Low;
  Low.Entry = F.Med.Entry;
  LowBlock B;
  B.Id = 0;
  B.StartAddr = Low.Entry;
  B.EndAddr = Low.Entry + 20;
  for (unsigned I = 0; I != 2; ++I) {
    LowOp Compute;
    Compute.Addr = Low.Entry + I * 4;
    Compute.Opcode = I ? NdOp::FLOAT_SUB : NdOp::FLOAT_ADD;
    Compute.Output = NdVar::tmp(I, 8);
    Compute.addInput(NdVar::reg(a64reg::V(0), 8));
    Compute.addInput(NdVar::reg(a64reg::V(1), 8));
    B.Ops.push_back(Compute);
  }
  for (unsigned I = 0; I != 2; ++I) {
    LowOp Write;
    Write.Addr = Low.Entry + 8 + I * 4;
    Write.Opcode = NdOp::INT_ZEXT;
    Write.Output = NdVar::reg(a64reg::V(I), 16);
    Write.addInput(NdVar::tmp(I, 8));
    B.Ops.push_back(Write);
  }
  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Low.Entry + 16;
  Return.addInput(NdVar::reg(a64reg::X30, 8));
  B.Ops.push_back(Return);
  Low.Blocks = {B};
  const auto Bind = [&](const SourceFunctionTypeHint &Hint) {
    std::map<va_t, SourceFunctionTypeHint> Hints{{Low.Entry, Hint}};
    LowToMedConverter Converter;
    Converter.setSourceCallHintsEnabled(true);
    Converter.setSourceCalleeTypeHints(&Hints);
    Converter.setSourceEntryTypeHints(&Hints);
    F.Med = Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
    F.Med.SourceTypeHint = Hint;
    recoverCallAbi(F.Med, Arch::AArch64, {});
    inferMedTypes(F.Med, Arch::AArch64);
    F.High = MedToHighConverter().convert(F.Med, Arch::AArch64);
  };
  Bind(Scalar);
  const auto Pair = refineNativeFloatingPairReturnHint(F.Med, F.High, F.Audit);
  ASSERT_TRUE(Pair);
  Bind(*Pair);
  ASSERT_TRUE(F.Med.SourceParametersBound);
  EXPECT_TRUE(equalSourceTypes(F.High.ReturnType, Pair->ReturnType));
  const auto Limitation = sdk::sourceBodyLimitation(F.High, *Pair, &F.Audit);
  EXPECT_TRUE(Limitation.empty()) << Limitation;
  EXPECT_FALSE(refineNativeFloatingPairReturnHint(F.Med, F.High, F.Audit));
  unsigned Returns = 0;
  walkStmts(F.High.Body, [&](const HighStmt &S) {
    if (S.Kind != StmtKind::Return)
      return;
    ++Returns;
    ASSERT_TRUE(S.RetVal);
    EXPECT_TRUE(equalSourceTypes(S.RetVal->Type, Pair->ReturnType));
    ASSERT_EQ(S.RetVal->Operands.size(), 2U);
  });
  EXPECT_EQ(Returns, 1U);
  // A carrier write is only a candidate. Publication must still reject an
  // undefined low lane after rebuilding the complete record-return function.
  Low.Blocks[0].Ops[1].Inputs[1] = NdVar::tmp(99, 8);
  Bind(*Pair);
  EXPECT_FALSE(sdk::sourceBodyLimitation(F.High, *Pair, &F.Audit).empty());
}

TEST(NativeSourceHints, FourDoubleCallerDemandNeedsEveryUnchangedCarrier) {
  SourceFunctionTypeHint Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Signature.ReturnType =
      NdType::makeStruct(std::vector<TypeRef>(4, NdType::makeFloat(8)));
  std::string Error;
  ASSERT_TRUE(assignDarwinFixedSourceABI(Signature, Arch::AArch64, Error))
      << Error;
  LowFunc Caller;
  Caller.Entry = 0x1000;
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = Caller.Entry;
  Block.Ops = {NativeVoidFrameFixture::op(NdOp::CALL, NdVar::reg(a64reg::X0, 8),
                                          {NdVar::cst(0x3000, 8)}),
               NativeVoidFrameFixture::op(NdOp::RETURN, {},
                                          {NdVar::reg(a64reg::X30, 8)})};
  Caller.Blocks = {Block};
  EXPECT_EQ(observedNativeFourDoubleReturns(Caller, Signature),
            (std::set<va_t>{0x3000}));
  for (unsigned I = 0; I != 4; ++I) {
    auto Changed = Caller;
    Changed.Blocks[0].Ops.insert(
        Changed.Blocks[0].Ops.begin() + 1,
        NativeVoidFrameFixture::op(NdOp::COPY, NdVar::reg(a64reg::V(I), 4),
                                   {NdVar::scalar(0, 4)}));
    EXPECT_TRUE(observedNativeFourDoubleReturns(Changed, Signature).empty());
    // Unknown upper Q lanes are outside the four low-double observations.
    Changed.Blocks[0].Ops[1].Output.Offset += 8;
    EXPECT_EQ(observedNativeFourDoubleReturns(Changed, Signature),
              (std::set<va_t>{0x3000}));
  }
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Caller;
    auto ABI = Signature;
    if (Mutation == 0)
      ABI.Architecture = Arch::X64;
    if (Mutation == 1)
      ABI.ReturnComponents.pop_back();
    if (Mutation == 2)
      ABI.ReturnComponents[3].RegisterOffset = a64reg::V(4);
    if (Mutation == 3)
      ABI.ReturnComponents[2].ValueBytes = 4;
    if (Mutation == 4)
      Changed.Blocks[0].Ops[0].Opcode = NdOp::INDIR_CALL;
    if (Mutation == 5)
      Changed.Blocks[0].Ops[0].Inputs[0] = NdVar::reg(a64reg::X7, 8);
    if (Mutation == 6)
      Changed.Blocks[0].Ops.insert(
          Changed.Blocks[0].Ops.begin() + 1,
          NativeVoidFrameFixture::op(NdOp::INTRINSIC, {}, {}));
    if (Mutation == 7)
      Changed.Blocks[0].Ops.insert(
          Changed.Blocks[0].Ops.begin() + 1,
          NativeVoidFrameFixture::op(NdOp::BRANCH, {},
                                     {NdVar::cst(0x4000, 8)}));
    if (Mutation == 8)
      Changed.Blocks[0].ExceptionalSuccs.emplace_back();
    EXPECT_TRUE(observedNativeFourDoubleReturns(Changed, ABI).empty());
  }
}

TEST(NativeSourceHints, FourDoubleReturnRequiresEveryComputedLowLane) {
  for (unsigned Mutation = 0; Mutation != 13; ++Mutation) {
    SCOPED_TRACE(Mutation);
    NativeFloatingFixture F(Arch::AArch64, 8);
    const auto First = F.Med.Blocks[0].Ops[0];
    for (unsigned I = 1; I != 4; ++I) {
      auto Write = First;
      Write.Output.RegOff = a64reg::V(I);
      Write.Output.Id = 10 + I;
      F.Med.Blocks[0].Ops.insert(F.Med.Blocks[0].Ops.end() - 1, Write);
    }
    std::string Error;
    const auto Scalar = F.infer(Error);
    ASSERT_TRUE(Scalar) << Error;
    F.Med.SourceTypeHint = F.High.SourceTypeHint = *Scalar;
    F.Med.SourceParametersBound = true;
    auto &Ops = F.Med.Blocks[0].Ops;
    if (Mutation >= 1 && Mutation <= 4)
      Ops[Mutation - 1].Output.Size = 4;
    if (Mutation >= 5 && Mutation <= 8)
      Ops.erase(Ops.begin() + Mutation - 5);
    if (Mutation == 9)
      F.Audit.MedIRVerified = false;
    if (Mutation == 10)
      F.Med.SourceParametersBound = false;
    if (Mutation == 11)
      F.Med.SourceTypeHint->Origin = F.High.SourceTypeHint->Origin =
          SourceFunctionTypeHint::OriginKind::DarwinSDK;
    if (Mutation == 12) {
      MedOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(MedVar::makeConst(0x1080, 8));
      Ops.insert(Ops.end() - 1, Call);
    }
    const auto Record =
        refineNativeFourDoubleReturnHint(F.Med, F.High, F.Audit);
    EXPECT_EQ(bool(Record), Mutation == 0);
    if (Record) {
      ASSERT_EQ(Record->ReturnComponents.size(), 4U);
      EXPECT_EQ(Record->ReturnType->Size, 32U);
      for (unsigned I = 0; I != 4; ++I) {
        EXPECT_EQ(Record->ReturnComponents[I].RegisterOffset, a64reg::V(I));
        EXPECT_EQ(Record->ReturnComponents[I].ValueBytes, 8U);
      }
      EXPECT_TRUE(equalSourceABIs(*F.Med.SourceTypeHint, *Scalar));
    }
  }
}

TEST(NativeSourceHints,
     FloatingRecordCallsKeepUpperLanesUnknownAndEvaluateOnce) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Width : {4U, 8U})
      for (unsigned Count : {2U, 4U})
        for (unsigned Lane = 0; Lane != Count; ++Lane)
          for (bool Upper : {false, true}) {
            if (Architecture == Arch::X64 &&
                (Width != 8 || Count != 4 || Lane == 3))
              continue;
            SCOPED_TRACE(::testing::Message()
                         << unsigned(Architecture) << ":" << Width << ":"
                         << Count << ":" << Lane << ":" << Upper);
            const auto &TRI = getTargetRegInfo(Architecture);
            NativeFixture F(Architecture);
            F.Audit.DecodedInstructions = F.Audit.LiftedInstructions = 4;
            SourceFunctionTypeHint Entry;
            Entry.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
            Entry.ReturnType = NdType::makeFloat(Width);
            std::string Error;
            ASSERT_TRUE(assignDarwinFixedSourceABI(Entry, Architecture, Error))
                << Error;
            auto Callee = Entry;
            std::vector<TypeRef> Members(Count, NdType::makeFloat(Width));
            if (Architecture == Arch::X64)
              Members.back() = NdType::makePtr(NdType::makeVoid());
            Callee.ReturnType = NdType::makeStruct(Members);
            ASSERT_TRUE(
                Architecture == Arch::X64
                    ? assignDarwinSwiftSourceABI(Callee, Architecture, Error)
                    : assignDarwinFixedSourceABI(Callee, Architecture, Error))
                << Error;
            LowFunc Low;
            Low.Entry = F.Med.Entry;
            LowBlock B;
            B.Id = 0;
            B.StartAddr = Low.Entry;
            B.EndAddr = Low.Entry + 16;
            LowOp Call;
            Call.Opcode = NdOp::CALL;
            Call.Addr = Low.Entry;
            Call.addInput(NdVar::cst(0x1080, 8));
            B.Ops.push_back(Call);
            LowOp Extract;
            Extract.Opcode = NdOp::SUBBYTES;
            Extract.Addr = Low.Entry + 4;
            Extract.Output = NdVar::tmp(0, Width);
            Extract.addInput(NdVar::reg(TRI.FPParamRegs[Lane], 16));
            Extract.addInput(NdVar::cst(Upper ? Width : 0, 8));
            B.Ops.push_back(Extract);
            LowOp Write;
            Write.Opcode = NdOp::INT_ZEXT;
            Write.Addr = Low.Entry + 8;
            Write.Output = NdVar::reg(TRI.FPReturnReg, 16);
            Write.addInput(Extract.Output);
            B.Ops.push_back(Write);
            LowOp Return;
            Return.Opcode = NdOp::RETURN;
            Return.Addr = Low.Entry + 12;
            Return.addInput(NdVar::reg(
                TRI.LinkRegister ? TRI.LinkRegister : TRI.IntReturnReg, 8));
            B.Ops.push_back(Return);
            Low.Blocks = {B};
            std::map<va_t, SourceFunctionTypeHint> Hints{{Low.Entry, Entry},
                                                         {0x1080, Callee}};
            LowToMedConverter Converter;
            Converter.setSourceCallHintsEnabled(true);
            Converter.setSourceCalleeTypeHints(&Hints);
            Converter.setSourceEntryTypeHints(&Hints);
            F.Med = Converter.convert(Low, Architecture, BinaryFormat::MachO);
            F.Med.SourceTypeHint = Entry;
            recoverCallAbi(F.Med, Architecture, {{0x1080, "two_doubles"}});
            inferMedTypes(F.Med, Architecture);
            F.High = MedToHighConverter().convert(F.Med, Architecture);
            unsigned Calls = 0;
            const auto Limitation = sdk::sourceBodyLimitation(
                F.High, Entry, &F.Audit, [](const HighExpr &) { return true; });
            EXPECT_EQ(Limitation.empty(), !Upper) << Limitation;
            walkStmts(F.High.Body, [&](const HighStmt &S) {
              forEachExpr(S, [&](const ExprPtr &E) {
                const auto Visit = [&](const auto &Self,
                                       const ExprPtr &X) -> void {
                  if (!X)
                    return;
                  if (X->SourceCallHint)
                    ++Calls;
                  for (const auto &Operand : X->Operands)
                    Self(Self, Operand);
                };
                Visit(Visit, E);
              });
            });
            EXPECT_EQ(Calls, 1U);
          }
}

TEST(NativeSourceHints, FloatingLanesPreserveScalarParametersAndResults) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Width : {4U, 8U})
      for (bool Wide : {false, true}) {
        NativeFloatingFixture Fixture(Architecture, Width, Wide);
        std::string Error;
        const auto Hint = Fixture.infer(Error);
        ASSERT_TRUE(Hint) << Error;
        EXPECT_EQ(Hint->ReturnLocation.Kind,
                  SourceABICarrierKind::FloatingRegister);
        EXPECT_EQ(Hint->ReturnLocation.RegisterOffset,
                  getTargetRegInfo(Architecture).FPReturnReg);
        EXPECT_EQ(Hint->ReturnLocation.ValueBytes, Width);
        ASSERT_EQ(Hint->Parameters.size(), 2U);
        const auto Bytes = observedMedSourceEntryBytes(Fixture.Med, *Hint);
        ASSERT_TRUE(Bytes);
        for (unsigned I = 0; I < 2; ++I) {
          EXPECT_EQ(Bytes->at(getTargetRegInfo(Architecture).FPParamRegs[I]),
                    (uint64_t(1) << Width) - 1);
          EXPECT_EQ(Hint->Parameters[I].Type->Kind, NdTypeKind::Float);
          EXPECT_EQ(Hint->Parameters[I].Location.RegisterOffset,
                    getTargetRegInfo(Architecture).FPParamRegs[I]);
          EXPECT_EQ(Hint->Parameters[I].Location.ValueBytes, Width);
          EXPECT_EQ(Fixture.Med.TypedParams[I].Type->Kind, NdTypeKind::Int);
          EXPECT_EQ(Fixture.Med.Params[I].Size, Wide ? 16 : Width);
        }
        EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                  MedReturnValueEvidence::Unknown);
      }
}

TEST(NativeSourceHints, FloatingReturnPathsRequireCompleteDefinedLanes) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      NativeFloatingFixture Fixture(Architecture, 8);
      auto &Ops = Fixture.Med.Blocks[0].Ops;
      switch (Mutation) {
      case 0:
        Ops[0].Output.Size = 4;
        break;
      case 1:
        Ops[0].Output.RegOff = getTargetRegInfo(Architecture).IntReturnReg;
        Fixture.Med.Params[0].RegOff =
            getTargetRegInfo(Architecture).FPParamRegs[2];
        Ops[0].Inputs[0] = Fixture.Med.Params[0];
        break;
      case 2:
        Ops[0].Output.RegOff = getTargetRegInfo(Architecture).FPParamRegs[1];
        Fixture.Med.Params[0].RegOff =
            getTargetRegInfo(Architecture).FPParamRegs[2];
        Ops[0].Inputs[0] = Fixture.Med.Params[0];
        break;
      case 3: {
        MedOp Call;
        Call.Opcode = NdOp::CALL;
        Call.addInput(MedVar::makeConst(0x1080, 8));
        auto Hint = std::make_shared<SourceCallTypeHint>();
        Hint->Signature.ReturnType = NdType::makeVoid();
        std::string Error;
        ASSERT_TRUE(
            assignDarwinScalarSourceABI(Hint->Signature, Architecture, Error));
        Call.SourceCallHint = std::move(Hint);
        Ops.insert(Ops.end() - 1, Call);
        break;
      }
      case 4: {
        const auto Return = Ops.back();
        Fixture.Med.Blocks.emplace_back();
        Fixture.Med.Blocks.back().Id = 1;
        Fixture.Med.Blocks.back().Ops = {Return};
        break;
      }
      }
      std::string Error;
      EXPECT_FALSE(Fixture.infer(Error)) << Mutation << ": " << Error;
    }
}

TEST(NativeSourceHints, FloatingParametersRequireProvenEntryBytes) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      NativeFloatingFixture Fixture(Architecture, 8, true);
      auto &Ops = Fixture.Med.Blocks[0].Ops;
      switch (Mutation) {
      case 0: {
        MedOp Store;
        Store.Opcode = NdOp::STORE;
        Store.addInput(MedVar::makeConst(0x1080, 8));
        Store.addInput(Fixture.Med.Params[0]);
        Ops.insert(Ops.end() - 1, Store);
        break;
      }
      case 1:
        Ops[0].Inputs[1].ConstVal = 8;
        break;
      case 2:
        Fixture.Med.Params[0].RegOff =
            getTargetRegInfo(Architecture).IntParamRegs[0];
        break;
      case 3:
        Fixture.Med.Params[0].RegOff = Fixture.Med.Params[1].RegOff;
        break;
      case 4:
        Fixture.Med.TypedParams[0].Type = NdType::makePtr(NdType::makeVoid());
        break;
      case 5:
        Ops[0].Inputs[0].Size = 4;
        break;
      }
      std::string Error;
      if (Architecture == Arch::AArch64 && Mutation == 0) {
        const auto Hint = Fixture.infer(Error);
        ASSERT_TRUE(Hint) << Error;
        ASSERT_FALSE(Hint->Parameters.empty());
        EXPECT_EQ(Hint->Parameters[0].Type->SourceName,
                  kSourceAArch64Vector128CType);
        EXPECT_EQ(Hint->Parameters[0].Location.ValueBytes, 16U);
        continue;
      }
      EXPECT_FALSE(Fixture.infer(Error)) << Mutation << ": " << Error;
    }
}

TEST(NativeSourceHints, TwoFullQInputsRequireIndependentEntryByteProofs) {
  for (bool FullSecond : {false, true}) {
    NativeFloatingFixture Fixture(Arch::AArch64, 8, true);
    auto &Ops = Fixture.Med.Blocks[0].Ops;
    for (unsigned Index = 0; Index != (FullSecond ? 2U : 1U); ++Index) {
      MedOp Store;
      Store.Opcode = NdOp::STORE;
      Store.addInput(MedVar::makeConst(0x1080 + Index * 16, 8));
      Store.addInput(Fixture.Med.Params[Index]);
      Ops.insert(Ops.end() - 1, Store);
    }
    std::string Error;
    const auto Hint = Fixture.infer(Error);
    ASSERT_TRUE(Hint) << Error;
    ASSERT_EQ(Hint->Parameters.size(), 2U);
    EXPECT_EQ(Hint->Parameters[0].Type->SourceName,
              kSourceAArch64Vector128CType);
    EXPECT_EQ(Hint->Parameters[0].Location.ValueBytes, 16U);
    EXPECT_EQ(Hint->Parameters[1].Type->SourceName,
              FullSecond ? kSourceAArch64Vector128CType : "");
    EXPECT_EQ(Hint->Parameters[1].Location.ValueBytes, FullSecond ? 16U : 8U);
    for (unsigned Index = 0; Index != 2; ++Index)
      EXPECT_EQ(Hint->Parameters[Index].Location.RegisterOffset,
                getTargetRegInfo(Arch::AArch64).FPParamRegs[Index]);
  }
}

TEST(NativeSourceHints,
     FloatingParameterEffectsSurviveAnUnprovedProvisionalReturn) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeVoidFixture Fixture(Architecture);
    const auto &TRI = getTargetRegInfo(Architecture);
    auto &Parameter = Fixture.Med.Params[1];
    Parameter.Id = 1;
    Parameter.RegOff = TRI.FPParamRegs[0];
    Parameter.Size = 16;
    Parameter.TheArch = Architecture;
    Fixture.Med.TypedParams[1].Type = Fixture.High.Params[1].Type =
        NdType::makeInt(16);

    auto Binding = std::make_shared<SourceCallTypeHint>(
        *Fixture.Med.Blocks[0].Ops[0].SourceCallHint);
    Binding->Signature.Parameters.push_back({"value", NdType::makeFloat(8)});
    std::string Error;
    ASSERT_TRUE(
        assignDarwinScalarSourceABI(Binding->Signature, Architecture, Error));

    MedOp Extract;
    Extract.Opcode = NdOp::SUBBYTES;
    Extract.Output.Kind = MedVar::Temp;
    Extract.Output.Id = 80;
    Extract.Output.Size = 8;
    Extract.Output.TheArch = Architecture;
    Extract.addInput(Parameter);
    Extract.addInput(MedVar::makeConst(0, 8));
    auto &Call = Fixture.Med.Blocks[0].Ops[0];
    Call.SourceCallHint = std::move(Binding);
    Call.addInput(Extract.Output);
    Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin(),
                                     std::move(Extract));

    const auto Hint = Fixture.inferVoid(Error);
    ASSERT_TRUE(Hint) << unsigned(Architecture) << ": " << Error;
    EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
    ASSERT_EQ(Hint->Parameters.size(), 2U);
    EXPECT_EQ(Hint->Parameters[1].Type->Kind, NdTypeKind::Float);
    EXPECT_EQ(Hint->Parameters[1].Type->Size, 8U);
    EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, TRI.FPParamRegs[0]);
    EXPECT_EQ(Hint->Parameters[1].Location.ValueBytes, 8U);
  }
}

struct NativeRecordResultFixture : NativeFixture {
  explicit NativeRecordResultFixture(Arch Architecture, bool Swift = false)
      : NativeFixture(Architecture) {
    Med.Params.clear();
    Med.TypedParams.clear();
    High.Params.clear();
    Med.ReturnType = High.ReturnType = NdType::makeInt(8, false);
    auto Hint = std::make_shared<SourceCallTypeHint>();
    Hint->CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
    Hint->Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinRuntime;
    Hint->Signature.ReturnType = NdType::makeStruct(
        {NdType::makePtr(NdType::makeVoid()), NdType::makeInt(8, false)});
    std::string Error;
    const bool Assigned =
        Swift
            ? assignDarwinSwiftSourceABI(Hint->Signature, Architecture, Error)
            : assignDarwinFixedSourceABI(Hint->Signature, Architecture, Error);
    EXPECT_TRUE(Assigned) << Error;
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Output.Kind = MedVar::Temp;
    Call.Output.Id = 100;
    Call.Output.SSAVer = 1;
    Call.Output.Size = 16;
    Call.Output.TheArch = Architecture;
    Call.SourceCallHint = Hint;
    Call.addInput(MedVar::makeConst(0x1080, 8));
    const auto Return = Med.Blocks[0].Ops.back();
    Med.Blocks[0].Ops = {Call};
    const auto &TRI = getTargetRegInfo(Architecture);
    for (unsigned I = 0; I < 2; ++I) {
      MedOp Extract;
      Extract.Opcode = NdOp::SUBBYTES;
      Extract.Output = Call.Output;
      Extract.Output.Kind = MedVar::Reg;
      Extract.Output.Id = 101 + I;
      Extract.Output.Size = 8;
      Extract.Output.RegOff = TRI.IntReturnRegs[I];
      Extract.addInput(Call.Output);
      Extract.addInput(MedVar::makeConst(I * 8, 8));
      Med.Blocks[0].Ops.push_back(Extract);
    }
    Med.Blocks[0].Ops.push_back(Return);
  }
};

TEST(NativeSourceHints, BoundRecordArgumentsUsePhysicalComponents) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool PairResult : {false, true})
      for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
        SCOPED_TRACE(Mutation);
        NativeRecordResultFixture F(Architecture);
        auto &Call = F.Med.Blocks[0].Ops[0];
        auto Binding =
            std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
        Binding->Signature.Parameters = {
            {"range", NdType::makeStruct({NdType::makeInt(8, false),
                                          NdType::makeInt(8, false)})}};
        std::string Error;
        ASSERT_TRUE(assignDarwinFixedSourceABI(Binding->Signature, Architecture,
                                               Error));
        Call.addInput(MedVar::makeConst(17, 8));
        if (Mutation != 1)
          Call.addInput(MedVar::makeConst(31, 8));
        if (Mutation == 2)
          Call.addInput(MedVar::makeConst(91, 8));
        if (Mutation == 3)
          Binding->Signature.Parameters[0].Components.pop_back();
        Call.SourceCallHint = Binding;
        const auto Hint = inferNativeSourceTypeHint(
            F.Image, F.Med, F.High, F.Audit, Error, nullptr, PairResult);
        EXPECT_EQ(bool(Hint), Mutation == 0) << Error;
        if (Hint)
          EXPECT_EQ(Hint->ReturnComponents.size(), PairResult ? 2U : 0U);
      }
}

TEST(NativeSourceHints, TypedRecordCallResultsDefineTheNativeReturnCarrier) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Swift : {false, true})
      for (bool SeparateReturn : {false, true}) {
        NativeRecordResultFixture Fixture(Architecture, Swift);
        if (SeparateReturn) {
          const auto Return = Fixture.Med.Blocks[0].Ops.back();
          Fixture.Med.Blocks[0].Ops.pop_back();
          Fixture.Med.Blocks[0].Succs = {1};
          Fixture.Med.Blocks.emplace_back();
          Fixture.Med.Blocks[1].Id = 1;
          Fixture.Med.Blocks[1].Preds = {0};
          Fixture.Med.Blocks[1].Ops = {Return};
        }
        std::string Error;
        const auto Hint = Fixture.infer(Error);
        ASSERT_TRUE(Hint) << Error;
        EXPECT_EQ(Hint->ReturnLocation.RegisterOffset,
                  getTargetRegInfo(Architecture).IntReturnReg);
        EXPECT_EQ(Hint->ReturnLocation.ValueBytes, 8U);
        EXPECT_TRUE(Hint->Parameters.empty());
        EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                  MedReturnValueEvidence::Unknown);
      }
}

TEST(NativeSourceHints, IntegerPairReturnsPreserveBothWordsOfBoundRecordCalls) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Swift : {false, true}) {
      NativeRecordResultFixture F(Architecture, Swift);
      std::string Error;
      const auto Pair = inferNativeSourceTypeHint(
          F.Image, F.Med, F.High, F.Audit, Error, nullptr, true);
      ASSERT_TRUE(Pair) << Error;
      EXPECT_EQ(Pair->ReturnType->Kind, NdTypeKind::Struct);
      EXPECT_EQ(Pair->ReturnComponents.size(), 2U);
      // A later scalar call clobbers the second result even when its first
      // result is a complete eight-byte value.
      auto Call = F.Med.Blocks[0].Ops[0];
      auto Binding = std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
      Binding->Signature.ReturnType = NdType::makeInt(8);
      ASSERT_TRUE(
          assignDarwinFixedSourceABI(Binding->Signature, Architecture, Error));
      Call.SourceCallHint = Binding;
      Call.Output = F.Med.Blocks[0].Ops[1].Output;
      F.Med.Blocks[0].Ops.insert(F.Med.Blocks[0].Ops.end() - 1, Call);
      const auto Scalar = inferNativeSourceTypeHint(
          F.Image, F.Med, F.High, F.Audit, Error, nullptr, true);
      ASSERT_TRUE(Scalar) << Error;
      EXPECT_TRUE(Scalar->ReturnComponents.empty());
    }
}

TEST(NativeSourceHints, StringWordsStayDefinedWhenTheCalleeAlsoReturnsBoolean) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
      SCOPED_TRACE(Mutation);
      NativeRecordResultFixture F(Architecture, true);
      auto &Ops = F.Med.Blocks[0].Ops;
      auto Binding =
          std::make_shared<SourceCallTypeHint>(*Ops[0].SourceCallHint);
      const auto Boolean = NdType::makeInt(1, false);
      Boolean->SourceName = kSourceBooleanCType;
      Binding->Signature.ReturnType =
          NdType::makeStruct({NdType::makeInt(8, false),
                              NdType::makePtr(NdType::makeVoid()), Boolean});
      std::string Error;
      ASSERT_TRUE(
          assignDarwinSwiftSourceABI(Binding->Signature, Architecture, Error));
      Ops[0].SourceCallHint = Binding;
      Ops[0].Output.Size = 24;
      for (unsigned I = 1; I != 3; ++I)
        Ops[I].Inputs[0].Size = 24;
      // No byte-valued definition of the third carrier is needed or allowed
      // by this proof of just the two full String words.
      if (Mutation == 1)
        Binding->Signature.ReturnComponents[2].Kind =
            SourceABICarrierKind::IntegerRegister;
      if (Mutation == 2)
        Binding->Signature.ReturnComponents.pop_back();
      if (Mutation == 3)
        Ops[0].Output.Size = 16;
      if (Mutation == 4)
        Ops[2].Inputs[0].Size = 16;
      if (Mutation == 5)
        Ops[2].Output.Size = 4;
      if (Mutation == 6)
        Ops[2].Inputs[1].ConstVal = 16;
      if (Mutation == 7)
        Ops[1].Output.Size = 4;
      if (Mutation == 8) {
        Binding->Signature.ReturnType = NdType::makeInt(16, false);
        ASSERT_TRUE(assignDarwinSwiftSourceABI(Binding->Signature, Architecture,
                                               Error));
      }
      const auto Pair = inferNativeSourceTypeHint(
          F.Image, F.Med, F.High, F.Audit, Error, nullptr, true);
      if (Mutation == 0) {
        ASSERT_TRUE(Pair) << Error;
        EXPECT_EQ(Pair->ReturnType->Size, 16U);
        EXPECT_EQ(Pair->ReturnComponents.size(), 2U);
        EXPECT_EQ(Ops[0].SourceCallHint->Signature.ReturnType->Size, 24U);
        EXPECT_EQ(Ops[0].SourceCallHint->Signature.ReturnComponents.size(), 3U);
      } else {
        EXPECT_FALSE(Pair) << Error;
      }
    }
}

TEST(NativeSourceHints, RecordResultDefinitionsRequireTheCompleteCallPrefix) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 15; ++Mutation) {
      SCOPED_TRACE(Mutation);
      NativeRecordResultFixture Fixture(Architecture);
      auto &Ops = Fixture.Med.Blocks[0].Ops;
      switch (Mutation) {
      case 0:
        Ops.erase(Ops.begin() + 2);
        break;
      case 1:
        std::swap(Ops[1], Ops[2]);
        break;
      case 2:
        Ops[1].Inputs[1].ConstVal = 8;
        break;
      case 3:
        ++Ops[1].Inputs[0].Id;
        break;
      case 4:
        ++Ops[1].Inputs[0].SSAVer;
        break;
      case 5:
        Ops[1].Inputs[0].Size = 8;
        break;
      case 6:
        Ops[1].Output.Size = 4;
        break;
      case 7:
        Ops[2].Output.Size = 4;
        break;
      case 8:
        Ops[1].Output.RegOff = getTargetRegInfo(Architecture).IntReturnRegs[1];
        break;
      case 9:
        Ops[0].Output.Size = 8;
        break;
      case 10:
        Ops[0].SourceCallHint.reset();
        break;
      case 11: {
        auto Hint =
            std::make_shared<SourceCallTypeHint>(*Ops[0].SourceCallHint);
        Hint->Signature.ReturnComponents.pop_back();
        Ops[0].SourceCallHint = std::move(Hint);
        break;
      }
      case 12: {
        auto Copy = Ops[1];
        Copy.Opcode = NdOp::COPY;
        Copy.Output = Ops[0].Output;
        Copy.Output.Id = 200;
        Copy.NumInputs = 1;
        Ops.insert(Ops.begin() + 2, Copy);
        break;
      }
      case 13: {
        auto Clobber = Ops[0];
        auto Hint = std::make_shared<SourceCallTypeHint>();
        Hint->Signature.ReturnType = NdType::makeVoid();
        std::string Error;
        ASSERT_TRUE(
            assignDarwinScalarSourceABI(Hint->Signature, Architecture, Error));
        Clobber.SourceCallHint = std::move(Hint);
        Clobber.Output = {};
        Ops.insert(Ops.end() - 1, Clobber);
        break;
      }
      case 14:
        Ops[2].Inputs[1].ConstVal = 0;
        break;
      }
      std::string Error;
      EXPECT_FALSE(Fixture.infer(Error)) << Error;
    }
}

struct NativeTerminalContextFixture : NativeFixture {
  LowFunc Low;
  static constexpr va_t ImportSlot = 0x2000;
  static constexpr uint64_t Context = a64reg::X20;

  NativeTerminalContextFixture() : NativeFixture(Arch::AArch64) {
    const std::string Import =
        "_$ss17_assertionFailure__4file4line5flagss5NeverOs12StaticStringV_"
        "SSAHSus6UInt32VtF";
    Segment Data;
    Data.VA = ImportSlot;
    Data.Size = Data.FileSz = 8;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(8);
    Image.Segments.push_back(std::move(Data));
    Image.ImportPtrSlots[ImportSlot] = Import;
    EXPECT_TRUE(Image.recordDyldBindSlot(
        ImportSlot, Import, 0, "/usr/lib/swift/libswiftCore.dylib", false));
    const auto Runtime = swiftRuntimeSourceCallHint(Image, ImportSlot);
    EXPECT_TRUE(Runtime);
    if (!Runtime)
      return;
    Med.Params.clear();
    Med.TypedParams.clear();
    High.Params.clear();
    Med.DoesNotReturn = High.DoesNotReturn = true;
    MedVar ContextValue;
    ContextValue.Kind = MedVar::Reg;
    ContextValue.Id = 200;
    ContextValue.RegOff = Context;
    ContextValue.Size = 8;
    ContextValue.TheArch = Arch::AArch64;
    MedOp Store;
    Store.Opcode = NdOp::STORE;
    Store.Addr = 0x1004;
    Store.addInput(ContextValue);
    Store.addInput(MedVar::makeConst(0, 8));
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = 0x1010;
    Call.OriginSeq = 0;
    Call.DoesNotReturn = true;
    Call.SourceCallHint = std::make_shared<const SourceCallTypeHint>(*Runtime);
    // Import metadata names the data slot; machine calls name the veneer.
    Call.addInput(MedVar::makeConst(0x1080, 8));
    std::vector<ExprPtr> Arguments;
    for (const auto &Parameter : Runtime->Signature.Parameters) {
      Call.addInput(MedVar::makeConst(0, Parameter.Type->Size));
      Arguments.push_back(HighExpr::makeConst(0, Parameter.Type->Size));
    }
    Med.Blocks[0].Ops = {Store, Call};
    HighStmt Statement;
    Statement.Kind = StmtKind::Call;
    Statement.CallExpr =
        HighExpr::makeCall(Runtime->TargetName, 0x1080, std::move(Arguments));
    Statement.CallExpr->Type = NdType::makeVoid();
    Statement.CallExpr->SourceCallHint = Call.SourceCallHint;
    High.Body = {Statement};
    Low.Entry = Med.Entry;
    Low.Blocks.emplace_back();
    Low.Blocks[0].Id = 0;
    using F = NativeVoidFrameFixture;
    const auto SP = NdVar::reg(a64reg::SP, 8);
    const auto Address = NdVar::tmp(TmpBase, 8);
    Low.Blocks[0].Ops = {
        F::op(NdOp::INT_SUB, SP, {SP, NdVar::cst(32, 8)}, 0x1000),
        F::op(NdOp::STORE, {}, {NdVar::reg(Context, 8), NdVar::cst(0, 8)},
              0x1004),
        F::op(NdOp::STORE, {}, {SP, NdVar::cst(115, 8)}, 0x1008),
        F::op(NdOp::INT_ADD, Address, {SP, NdVar::cst(8, 8)}, 0x100c),
        F::op(NdOp::STORE, {}, {Address, NdVar::cst(0, 4)}, 0x100c),
        F::op(NdOp::CALL, {}, {NdVar::cst(0x1080, 8)}, 0x1010)};
  }

  static constexpr va_t ReturningImportSlot = 0x2010;

  void addReturningRuntimeCall() {
    Segment Data;
    Data.VA = ReturningImportSlot;
    Data.Size = Data.FileSz = 8;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(8);
    Image.Segments.push_back(std::move(Data));
    const std::string Import = "_swift_unknownObjectWeakInit";
    Image.ImportPtrSlots[ReturningImportSlot] = Import;
    ASSERT_TRUE(Image.recordDyldBindSlot(ReturningImportSlot, Import, 0,
                                         "/usr/lib/swift/libswiftCore.dylib",
                                         false));
    const auto Runtime = swiftRuntimeSourceCallHint(Image, ReturningImportSlot);
    ASSERT_TRUE(Runtime);
    ASSERT_FALSE(Runtime->DoesNotReturn);
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = 0x1006;
    Call.OriginSeq = 0;
    Call.SourceCallHint = std::make_shared<const SourceCallTypeHint>(*Runtime);
    Call.addInput(MedVar::makeConst(0x1090, 8));
    for (const auto &Parameter : Runtime->Signature.Parameters)
      Call.addInput(MedVar::makeConst(0, Parameter.Type->Size));
    Med.Blocks[0].Ops.insert(Med.Blocks[0].Ops.begin() + 1, Call);
    Low.Blocks[0].Ops.insert(Low.Blocks[0].Ops.begin() + 2,
                             NativeVoidFrameFixture::op(NdOp::CALL, {},
                                                        {NdVar::cst(0x1090, 8)},
                                                        0x1006));
  }

  std::optional<SourceFunctionTypeHint> inferContext(std::string &Error) const {
    return inferNativeSourceTypeHint(Image, Med, High, Audit, Error, &Low);
  }
};

TEST(NativeSourceHints, TerminalEntryBytesRequireEffectsOnlyAndProvenExit) {
  NativeTerminalContextFixture Fixture;
  SourceFunctionTypeHint Signature;
  Signature.ReturnType = NdType::makeVoid();
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(Signature, Arch::AArch64, Error));
  EXPECT_FALSE(observedMedSourceEntryBytes(Fixture.Med, Signature));
  const auto Bytes = observedMedSourceEntryBytes(
      Fixture.Med, Signature, SourceEntryDemand::EffectsOnly);
  ASSERT_TRUE(Bytes);
  ASSERT_EQ(Bytes->size(), 1U);
  EXPECT_EQ(Bytes->at(Fixture.Context), 0xffU);
  for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
    auto Changed = Fixture.Med;
    if (Mutation == 0)
      Changed.DoesNotReturn = false;
    else if (Mutation == 1)
      Changed.Blocks[0].Ops.back().DoesNotReturn = false;
    else {
      Changed.Blocks[0].Ops.pop_back();
      if (Mutation == 3)
        Changed.Blocks[0].Succs = {99};
    }
    EXPECT_FALSE(observedMedSourceEntryBytes(Changed, Signature,
                                             SourceEntryDemand::EffectsOnly))
        << Mutation;
  }
}

TEST(NativeSourceHints, TerminalRuntimeRetainsAnObservedUnwrittenContext) {
  NativeTerminalContextFixture Fixture;
  std::string Error;
  const auto Hint = Fixture.inferContext(Error);
  ASSERT_TRUE(Hint) << Error;
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Hint->Parameters.size(), 1U);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, Fixture.Context);
  EXPECT_EQ(Hint->Parameters[0].Location.ValueBytes, 8U);

  const auto WithoutLow = Fixture.infer(Error);
  ASSERT_TRUE(WithoutLow) << Error;
  EXPECT_TRUE(WithoutLow->Parameters.empty());
}

TEST(NativeSourceHints, TerminalContextSurvivesAnAuthenticatedRuntimePrefix) {
  NativeTerminalContextFixture Fixture;
  Fixture.addReturningRuntimeCall();
  std::string Error;
  const auto Hint = Fixture.inferContext(Error);
  ASSERT_TRUE(Hint) << Error;
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Hint->Parameters.size(), 1U);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, Fixture.Context);
  EXPECT_EQ(Hint->Parameters[0].Location.ValueBytes, 8U);
}

TEST(NativeSourceHints, TerminalRuntimePrefixRequiresIndependentCallEvidence) {
  for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    NativeTerminalContextFixture F;
    F.addReturningRuntimeCall();
    auto &Ops = F.Low.Blocks[0].Ops;
    auto &MedOps = F.Med.Blocks[0].Ops;
    auto Binding =
        std::make_shared<SourceCallTypeHint>(*MedOps[1].SourceCallHint);
    MedOps[1].SourceCallHint = Binding;
    using Op = NativeVoidFrameFixture;
    const auto SP = NdVar::reg(a64reg::SP, 8);
    switch (Mutation) {
    case 0:
      F.Image.DyldBindSlots[F.ReturningImportSlot].Module =
          "/tmp/foreign.dylib";
      break;
    case 1:
      F.Image.DyldBindSlots.erase(F.ReturningImportSlot);
      break;
    case 2:
      Binding->TargetName += "_forged";
      break;
    case 3:
      Binding->CallKind = SourceCallTypeHint::Kind::Native;
      Binding->TargetAddress = 0x1090;
      Binding->Signature.Origin =
          SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      break;
    case 4:
      ++Ops[2].Seq;
      break;
    case 5:
      // The unique terminating call must be last even when all occurrences
      // still match their independent runtime declarations.
      std::swap(Ops[2], Ops.back());
      std::swap(MedOps[1], MedOps.back());
      break;
    case 6:
      // Two authentic terminal declarations cannot bypass the prefix proof.
      MedOps[1].SourceCallHint = MedOps.back().SourceCallHint;
      MedOps[1].DoesNotReturn = true;
      MedOps[1].Inputs = MedOps.back().Inputs;
      MedOps[1].NumInputs = MedOps.back().NumInputs;
      Ops[2].Inputs[0] = Ops.back().Inputs[0];
      break;
    case 7:
      // Declared stack bytes must all be written after prefix effects too.
      Ops[3].Inputs[1].Size = 4;
      break;
    case 8:
      // The returning prefix cannot borrow an arbitrary private frame value.
      Ops.insert(Ops.begin() + 2,
                 Op::op(NdOp::COPY, NdVar::reg(a64reg::X0, 8), {SP}, 0x1006));
      break;
    case 9:
      --MedOps[1].NumInputs;
      break;
    case 10:
      Binding->Signature.Parameters[0].Location.RegisterOffset = a64reg::X2;
      break;
    case 11:
      // A caller-clobbered copy cannot retain the entry identity across the
      // prefix call merely because the source graph still names X20.
      Ops.insert(Ops.begin() + 1, Op::op(NdOp::COPY, NdVar::reg(a64reg::X9, 8),
                                         {NdVar::reg(F.Context, 8)}, 0x1004));
      Ops[2].Inputs[0] = NdVar::cst(0x3000, 8);
      Ops.insert(Ops.begin() + 4,
                 Op::op(NdOp::STORE, {},
                        {NdVar::reg(a64reg::X9, 8), NdVar::cst(0, 8)}, 0x1008));
      break;
    }
    std::string Error;
    const auto Hint = F.inferContext(Error);
    if (Hint)
      EXPECT_TRUE(Hint->Parameters.empty()) << Error;
  }
}

TEST(NativeSourceHints, TerminalContextRequiresExactCallAndCompleteFrameProof) {
  for (unsigned Mutation = 0; Mutation < 27; ++Mutation) {
    SCOPED_TRACE(Mutation);
    NativeTerminalContextFixture F;
    auto &Ops = F.Low.Blocks[0].Ops;
    auto &Call = F.Med.Blocks[0].Ops.back();
    auto Binding = std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
    Call.SourceCallHint = Binding;
    const auto SP = NdVar::reg(a64reg::SP, 8);
    using Op = NativeVoidFrameFixture;
    switch (Mutation) {
    case 0:
      F.Image.DyldBindSlots[F.ImportSlot].Module = "/tmp/foreign.dylib";
      break;
    case 1:
      F.Image.DyldBindSlots.clear();
      break;
    case 2:
      Binding->TargetName += "_forged";
      break;
    case 3:
      Binding->CallKind = SourceCallTypeHint::Kind::Native;
      Binding->TargetAddress = 0x1080;
      Binding->Signature.Origin =
          SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      break;
    case 4:
      Call.DoesNotReturn = false;
      break;
    case 5:
      Binding->DoesNotReturn = false;
      break;
    case 6:
      ++Ops.back().Seq;
      break;
    case 7:
      Ops.back().Inputs[0].Offset += 4;
      break;
    case 8:
      Ops.back().Addr += 4;
      break;
    case 9:
      Ops.back().Opcode = NdOp::INDIR_CALL;
      break;
    case 10:
      Ops[1].Inputs[0].Size = 4;
      break;
    case 11:
    case 12:
      Ops.insert(Ops.begin() + 1,
                 Op::op(NdOp::COPY,
                        NdVar::reg(F.Context + (Mutation == 12),
                                   Mutation == 11 ? 8 : 1),
                        {NdVar::cst(0, Mutation == 11 ? 8 : 1)}, 0x1004));
      break;
    case 13:
      // A complete prologue spill is not a source-level context use.
      Ops[1] = Op::op(NdOp::STORE, {}, {SP, NdVar::reg(F.Context, 8)}, 0x1004);
      break;
    case 14:
      Ops[2].Inputs[1].Size = 4;
      break;
    case 15:
      Ops.erase(Ops.begin() + 4);
      break;
    case 16:
      Ops.insert(Ops.end() - 1,
                 Op::op(NdOp::COPY, NdVar::reg(a64reg::X0, 8), {SP}, 0x100c));
      break;
    case 17:
      Ops[1].Inputs[1] = SP;
      break;
    case 18:
      Ops.front().Inputs[1] = NdVar::cst(8, 8);
      break;
    case 19:
      F.Low.Blocks[0].Succs = {0};
      break;
    case 20:
      F.Low.Blocks[0].ExceptionalSuccs.emplace_back();
      break;
    case 21:
      Ops.insert(Ops.begin() + 1,
                 Op::op(NdOp::BRANCH, {}, {NdVar::cst(0x1010, 8)}, 0x1004));
      break;
    case 22:
      Ops.push_back(
          Op::op(NdOp::RETURN, {}, {NdVar::reg(a64reg::X30, 8)}, 0x1014));
      break;
    case 23:
      F.Low.Blocks.emplace_back();
      F.Low.Blocks.back().Id = 1;
      break;
    case 24:
      --Call.NumInputs;
      break;
    case 25:
      Binding->Signature.Parameters.back().Location.EntryStackOffset += 4;
      break;
    case 26:
      F.Med.DoesNotReturn = F.High.DoesNotReturn = false;
      break;
    }
    std::string Error;
    const auto Hint = F.inferContext(Error);
    if (Hint)
      EXPECT_TRUE(Hint->Parameters.empty()) << Error;
  }
}

struct NativeContextFixture : NativeFixture {
  LowFunc Low;
  MedVar Context;

  explicit NativeContextFixture(Arch Architecture, uint64_t Register)
      : NativeFixture(Architecture) {
    Med.Params[1].Id = -1;
    Context.Kind = MedVar::Reg;
    Context.Id = 200;
    Context.RegOff = Register;
    Context.Size = 8;
    Context.TheArch = Architecture;
    MedOp Load;
    Load.Opcode = NdOp::LOAD;
    Load.Output = Context;
    Load.Output.Kind = MedVar::Temp;
    Load.Output.Id = 201;
    Load.Output.Size = 4;
    Load.addInput(Context);
    Med.Blocks[0].Ops[0].Inputs[1] = Load.Output;
    Med.Blocks[0].Ops.insert(Med.Blocks[0].Ops.begin(), Load);
    Low.Entry = Med.Entry;
    Low.Blocks.emplace_back();
    Low.Blocks[0].Id = 0;
    LowOp NativeLoad;
    NativeLoad.Opcode = NdOp::LOAD;
    NativeLoad.Output = NdVar::tmp(TmpBase, 4);
    NativeLoad.addInput(NdVar::reg(Register, 8));
    Low.Blocks[0].Ops.push_back(NativeLoad);
  }

  std::optional<SourceFunctionTypeHint> inferContext(std::string &Error) const {
    return inferNativeSourceTypeHint(Image, Med, High, Audit, Error, &Low);
  }
};

TEST(NativeSourceHints, BoundPreservedEntryRetainsItsPhysicalSeed) {
  NativeContextFixture Fixture(Arch::AArch64, a64reg::X20);
  MedOp Seed;
  Seed.Opcode = NdOp::COPY;
  Seed.Output = Fixture.Context;
  Seed.addInput(Fixture.Context);
  Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin(), Seed);
  std::string Error;
  const auto Hint = Fixture.inferContext(Error);
  ASSERT_TRUE(Hint) << Error;
  ASSERT_EQ(Hint->Parameters.size(), 2U);
  MedVar Narrow = Fixture.Context;
  Narrow.Size = 4;
  MedOp Mask;
  Mask.Opcode = NdOp::INT_AND;
  Mask.Output.Kind = MedVar::Temp;
  Mask.Output.Id = 302;
  Mask.Output.Size = 4;
  Mask.addInput(Narrow);
  Mask.addInput(MedVar::makeConst(1, 4));
  Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin() + 1, Mask);
  Fixture.Med.SourceTypeHint = *Hint;
  inferMedTypes(Fixture.Med, Arch::AArch64);
  ASSERT_TRUE(Fixture.Med.SourceTypeHint);
  ASSERT_EQ(Fixture.Med.Params.size(), 2U);
  EXPECT_GE(Fixture.Med.Params[1].Id, 0);
  Fixture.High = MedToHighConverter().convert(Fixture.Med, Arch::AArch64);
  EXPECT_TRUE(sdk::sourceBodyLimitation(
                  Fixture.High, *Fixture.High.SourceTypeHint, &Fixture.Audit,
                  [](const HighExpr &) { return true; })
                  .empty());
}

TEST(NativeSourceHints, DirectContextUseSurvivesUnrelatedIncompleteDemand) {
  NativeContextFixture Fixture(Arch::AArch64, a64reg::X20);
  MedVar Unrelated;
  Unrelated.Kind = MedVar::Reg;
  Unrelated.Id = 900;
  Unrelated.SSAVer = 1;
  Unrelated.RegOff = a64reg::X2;
  Unrelated.Size = 8;
  Unrelated.TheArch = Arch::AArch64;
  MedOp Store;
  Store.Opcode = NdOp::STORE;
  Store.addInput(Unrelated);
  Store.addInput(MedVar::makeConst(0, 8));
  Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin(), Store);

  std::string Error;
  const auto Hint = Fixture.inferContext(Error);
  ASSERT_TRUE(Hint) << Error;
  ASSERT_EQ(Hint->Parameters.size(), 2U);
  EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, a64reg::X20);

  Fixture.Med.Blocks[0].Ops[1].Inputs[0].SSAVer = 1;
  const auto WithoutEntry = Fixture.inferContext(Error);
  ASSERT_TRUE(WithoutEntry) << Error;
  EXPECT_EQ(WithoutEntry->Parameters.size(), 1U);
}

TEST(NativeSourceHints, ReadOnlyContextsRetainObservedPreservedRegisters) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    unsigned Checked = 0;
    for (uint64_t Register : TRI.CalleeSaveRegs) {
      if (TRI.isFrameOrLinkReg(Register) || TRI.isVectorReg(Register))
        continue;
      NativeContextFixture Fixture(Architecture, Register);
      std::string Error;
      auto Hint = Fixture.inferContext(Error);
      ASSERT_TRUE(Hint) << Error;
      ASSERT_EQ(Hint->Parameters.size(), 2U) << Register;
      EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset,
                TRI.IntParamRegs[0]);
      EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, Register);
      EXPECT_EQ(Hint->Parameters[1].Location.ValueBytes, 8U);
      EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::C);
      EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                MedReturnValueEvidence::Unknown);
      auto WithoutNativeProof = Fixture.infer(Error);
      ASSERT_TRUE(WithoutNativeProof) << Error;
      EXPECT_EQ(WithoutNativeProof->Parameters.size(), 1U);
      ++Checked;
    }
    EXPECT_GT(Checked, 0U);
  }
}

TEST(NativeSourceHints, PreservedContextsAllowASeparateArchitecturalTrapPath) {
  const auto Context = a64reg::X20;
  LowFunc Function;
  Function.Entry = 0x1000;
  Function.Blocks.resize(3);
  Function.Blocks[0].Id = 0;
  Function.Blocks[0].StartAddr = Function.Entry;
  Function.Blocks[0].Succs = {1, 2};
  Function.Blocks[1].Id = 1;
  Function.Blocks[1].Preds = {0};
  Function.Blocks[2].Id = 2;
  Function.Blocks[2].Preds = {0};

  LowOp Read;
  Read.Opcode = NdOp::LOAD;
  Read.Output = NdVar::tmp(TmpBase, 8);
  Read.addInput(NdVar::reg(Context, 8));
  Function.Blocks[0].Ops.push_back(Read);
  LowOp Reverse;
  Reverse.Opcode = NdOp::INTRINSIC;
  Reverse.Output = NdVar::tmp(TmpBase + 8, 8);
  Reverse.addInput(NdVar::cst(static_cast<uint64_t>(Intrinsic::A64_Rbit), 2));
  Reverse.addInput(Read.Output);
  Function.Blocks[0].Ops.push_back(Reverse);

  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(NdVar::reg(a64reg::X30, 8));
  Function.Blocks[1].Ops.push_back(Return);

  LowOp Clobber;
  Clobber.Opcode = NdOp::COPY;
  Clobber.Output = NdVar::reg(Context, 8);
  Clobber.addInput(NdVar::cst(0, 8));
  Function.Blocks[2].Ops.push_back(Clobber);
  LowOp Trap;
  Trap.Opcode = NdOp::INTRINSIC;
  Trap.Output = NdVar::reg(a64reg::X0, 8);
  Trap.addInput(NdVar::cst(static_cast<uint64_t>(Intrinsic::Brk), 2));
  Function.Blocks[2].Ops.push_back(Trap);

  std::set<uint64_t> Used;
  EXPECT_TRUE(restoresNativeSourceState(Function, Arch::AArch64, {}, &Used));
  EXPECT_EQ(Used, std::set<uint64_t>{Context});

  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    auto Invalid = Function;
    auto &InvalidTrap = Invalid.Blocks[2].Ops.back();
    if (Mutation == 0)
      InvalidTrap.Inputs[0].Offset = static_cast<uint64_t>(Intrinsic::Svc);
    else if (Mutation == 1)
      InvalidTrap.NumInputs = 0;
    else if (Mutation == 2)
      InvalidTrap.addInput(NdVar::cst(0, 2));
    else if (Mutation == 3)
      Invalid.Blocks[2].Succs = {1};
    else
      Invalid.Blocks[2].Ops.push_back(Clobber);
    EXPECT_FALSE(
        restoresNativeSourceState(Invalid, Arch::AArch64, {}, nullptr));
  }
  EXPECT_FALSE(restoresNativeSourceState(Function, Arch::X64, {}, nullptr));
}

TEST(NativeSourceHints, ScalarBitReverseHasExactNativeABIEvidence) {
  NativeFixture Fixture(Arch::AArch64);
  Fixture.Med.Params[0].Size = 8;
  Fixture.Med.TypedParams[0].Type = Fixture.High.Params[0].Type =
      NdType::makeInt(8, false);
  Fixture.Med.Blocks[0].Ops[0].Inputs[0] = Fixture.Med.Params[0];
  MedOp Reverse;
  Reverse.Opcode = NdOp::INTRINSIC;
  Reverse.Output.Kind = MedVar::Temp;
  Reverse.Output.Id = 300;
  Reverse.Output.Size = 8;
  Reverse.addInput(
      MedVar::makeConst(static_cast<uint64_t>(Intrinsic::A64_Rbit), 2));
  Reverse.addInput(Fixture.Med.Params[0]);
  Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin(), Reverse);

  std::string Error;
  EXPECT_TRUE(Fixture.infer(Error)) << Error;
  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    auto Invalid = Fixture;
    auto &Op = Invalid.Med.Blocks[0].Ops[0];
    if (Mutation == 0)
      Op.Inputs[0].ConstVal = static_cast<uint64_t>(Intrinsic::A64_Rev64);
    else if (Mutation == 1)
      Op.Inputs[0].Size = 4;
    else if (Mutation == 2)
      Op.Inputs[1].Size = 4;
    else if (Mutation == 3)
      Op.Output.Size = 16;
    else
      Op.addInput(MedVar::makeConst(0, 8));
    EXPECT_FALSE(Invalid.infer(Error)) << Mutation;
  }
}

TEST(NativeSourceHints, AuxiliaryInputsAllowLaterCallerSavedWrites) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto Register =
        Architecture == Arch::AArch64 ? a64reg::X8 : x86reg::R10;
    for (bool RecordedParameter : {false, true}) {
      NativeContextFixture Fixture(Architecture, Register);
      if (RecordedParameter) {
        Fixture.Context.Kind = MedVar::Param;
        Fixture.Med.Params[1] = Fixture.Context;
        Fixture.Med.TypedParams[1].Type = NdType::makeInt(8);
        Fixture.High.Params[1].Type = NdType::makeInt(8);
        Fixture.Med.Blocks[0].Ops[0].Inputs[0] = Fixture.Context;
      }
      LowOp Write;
      Write.Opcode = NdOp::COPY;
      Write.Output = NdVar::reg(Register, 8);
      Write.addInput(NdVar::cst(7, 8));
      Fixture.Low.Blocks[0].Ops.push_back(Write);
      std::string Error;
      const auto Hint = Fixture.inferContext(Error);
      ASSERT_TRUE(Hint) << Error;
      ASSERT_EQ(Hint->Parameters.size(), 2U);
      EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, Register);
      EXPECT_EQ(Hint->Parameters[1].Location.ValueBytes, 8U);
      EXPECT_FALSE(Hint->Parameters[1].Location.ExtendTo32Bits);
      EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                MedReturnValueEvidence::Unknown);
      EXPECT_EQ(bool(Fixture.infer(Error)), !RecordedParameter);
    }
  }
}

TEST(NativeSourceHints, AuxiliaryParametersRequireCompleteNativeReadEvidence) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto Register =
        Architecture == Arch::AArch64 ? a64reg::X8 : x86reg::R10;
    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      NativeContextFixture Fixture(Architecture, Register);
      Fixture.Context.Kind = MedVar::Param;
      Fixture.Med.Params[1] = Fixture.Context;
      Fixture.Med.TypedParams[1].Type = NdType::makeInt(8);
      Fixture.High.Params[1].Type = NdType::makeInt(8);
      Fixture.Med.Blocks[0].Ops[0].Inputs[0] = Fixture.Context;
      if (Mutation == 0)
        Fixture.Low.Blocks[0].Ops[0].Inputs[0].Size = 4;
      else if (Mutation == 1)
        Fixture.Low.Blocks[0].Ops.clear();
      else if (Mutation == 2)
        ++Fixture.Low.Entry;
      else if (Mutation == 3)
        Fixture.Low.Blocks[0].Ops[0].NumInputs = static_cast<uint8_t>(
            std::size(Fixture.Low.Blocks[0].Ops[0].Inputs) + 1);
      else {
        Fixture.Med.Params[1].Size = 4;
        Fixture.Med.TypedParams[1].Type = NdType::makeInt(4);
      }
      std::string Error;
      EXPECT_FALSE(Fixture.inferContext(Error)) << Mutation;
    }
  }
}

TEST(NativeSourceHints, AuxiliaryCallClobbersCannotBecomeEntryParameters) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto Register =
        Architecture == Arch::AArch64 ? a64reg::X8 : x86reg::R10;
    NativeContextFixture Fixture(Architecture, Register);
    Fixture.Med.CallClobbers.push_back({Fixture.Context, 1});
    std::string Error;
    const auto Hint = Fixture.inferContext(Error);
    ASSERT_TRUE(Hint) << Error;
    EXPECT_EQ(Hint->Parameters.size(), 1U);
  }
}

TEST(NativeSourceHints,
     ContextInputsSurviveProvenRestorationOfScratchRegisters) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    std::vector<uint64_t> Preserved;
    for (auto R : TRI.CalleeSaveRegs)
      if (!TRI.isFrameOrLinkReg(R) && !TRI.isVectorReg(R))
        Preserved.push_back(R);
    ASSERT_GE(Preserved.size(), 2U);
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      SCOPED_TRACE(Mutation);
      NativeContextFixture F(Architecture, Preserved[0]);
      auto &Ops = F.Low.Blocks[0].Ops;
      LowOp Save;
      Save.Opcode = NdOp::COPY;
      Save.Output = NdVar::tmp(TmpBase + 16, 8);
      Save.addInput(NdVar::reg(Preserved[1], 8));
      LowOp Write;
      Write.Opcode = NdOp::COPY;
      Write.Output = NdVar::reg(Preserved[1], 8);
      Write.addInput(NdVar::cst(19, 8));
      LowOp Restore;
      Restore.Opcode = NdOp::COPY;
      Restore.Output = Write.Output;
      Restore.addInput(Save.Output);
      LowOp Return;
      Return.Opcode = NdOp::RETURN;
      if (Architecture == Arch::AArch64)
        Return.addInput(NdVar::reg(TRI.LinkRegister, 8));
      Ops.insert(Ops.begin(), Save);
      Ops.push_back(Write);
      if (Mutation == 2)
        Restore.Output.Size = Restore.Inputs[0].Size = 4;
      if (Mutation != 1)
        Ops.push_back(Restore);
      if (Mutation == 3) {
        Write.Output = NdVar::reg(TRI.StackPointer, 8);
        Ops.push_back(Write);
      }
      Ops.push_back(Return);
      if (Mutation == 4)
        F.Low.Blocks[0].ExceptionalSuccs.emplace_back();
      if (Mutation == 5) {
        F.Low.Blocks[0].Succs = {99};
      }
      std::string Error;
      const auto Hint = F.inferContext(Error);
      ASSERT_TRUE(Hint) << Error;
      EXPECT_EQ(Hint->Parameters.size(), Mutation == 0 ? 2U : 1U);
    }
  }
}

TEST(NativeSourceHints, RestoredContextRequiresANonPreservationEntryUse) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    const auto Context = *std::find_if(
        TRI.CalleeSaveRegs.begin(), TRI.CalleeSaveRegs.end(), [&](uint64_t R) {
          return !TRI.isFrameOrLinkReg(R) && !TRI.isVectorReg(R);
        });
    NativeContextFixture Fixture(Architecture, Context);
    auto &Ops = Fixture.Low.Blocks[0].Ops;
    LowOp Save;
    Save.Opcode = NdOp::COPY;
    Save.Output = NdVar::tmp(TmpBase + 16, 8);
    Save.addInput(NdVar::reg(Context, 8));
    LowOp Write;
    Write.Opcode = NdOp::COPY;
    Write.Output = NdVar::reg(Context, 8);
    Write.addInput(NdVar::cst(19, 8));
    LowOp Restore;
    Restore.Opcode = NdOp::COPY;
    Restore.Output = Write.Output;
    Restore.addInput(Save.Output);
    LowOp Return;
    Return.Opcode = NdOp::RETURN;
    if (Architecture == Arch::AArch64)
      Return.addInput(NdVar::reg(TRI.LinkRegister, 8));
    Ops.insert(Ops.begin(), Save);
    Ops.push_back(Write);
    Ops.push_back(Restore);
    Ops.push_back(Return);

    std::string Error;
    const auto Used = Fixture.inferContext(Error);
    ASSERT_TRUE(Used) << Error;
    ASSERT_EQ(Used->Parameters.size(), 2U);
    EXPECT_EQ(Used->Parameters[1].Location.RegisterOffset, Context);

    auto SpillOnly = Fixture;
    SpillOnly.Low.Blocks[0].Ops[1].Inputs[0] = NdVar::cst(0x2000, 8);
    const auto Omitted = SpillOnly.inferContext(Error);
    ASSERT_TRUE(Omitted) << Error;
    EXPECT_EQ(Omitted->Parameters.size(), 1U);
  }
}

TEST(NativeSourceHints, ContextProofRejectsClobbersAndIncompleteNativeReads) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    const auto ContextRegister = *std::find_if(
        TRI.CalleeSaveRegs.begin(), TRI.CalleeSaveRegs.end(), [&](uint64_t R) {
          return !TRI.isFrameOrLinkReg(R) && !TRI.isVectorReg(R);
        });
    for (unsigned Mutation = 0; Mutation < 9; ++Mutation) {
      NativeContextFixture Fixture(Architecture, ContextRegister);
      auto &Ops = Fixture.Low.Blocks[0].Ops;
      if (Mutation < 3) {
        LowOp Write;
        Write.Opcode = NdOp::COPY;
        const auto Offset =
            Mutation == 2 ? ContextRegister + 1 : ContextRegister;
        Write.Output = NdVar::reg(Offset, Mutation ? 1 : 8);
        Write.addInput(NdVar::cst(0, Write.Output.Size));
        Ops.push_back(Write);
      } else if (Mutation == 3) {
        ++Fixture.Low.Entry;
      } else if (Mutation == 4) {
        Ops[0].Inputs[0] = NdVar::cst(0, 8);
      } else if (Mutation == 5) {
        Ops[0].Inputs[0].Size = 4;
      } else if (Mutation == 6) {
        Fixture.Low.Blocks.resize(16385);
      } else if (Mutation == 7) {
        Ops[0].NumInputs = static_cast<uint8_t>(std::size(Ops[0].Inputs) + 1);
      } else {
        LowOp Write;
        Write.Opcode = NdOp::COPY;
        auto Other = std::find_if(TRI.CalleeSaveRegs.begin(),
                                  TRI.CalleeSaveRegs.end(), [&](uint64_t R) {
                                    return R != ContextRegister &&
                                           !TRI.isFrameOrLinkReg(R) &&
                                           !TRI.isVectorReg(R);
                                  });
        ASSERT_NE(Other, TRI.CalleeSaveRegs.end());
        Write.Output = NdVar::reg(*Other, 8);
        Write.addInput(NdVar::cst(0, 8));
        Ops.push_back(Write);
      }
      std::string Error;
      auto Hint = Fixture.inferContext(Error);
      ASSERT_TRUE(Hint) << Error;
      EXPECT_EQ(Hint->Parameters.size(), 1U) << Mutation;
    }
  }
}

TEST(NativeSourceHints, ContextDemandExcludesSeedsAndInternalDefinitions) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    const auto Register = *std::find_if(
        TRI.CalleeSaveRegs.begin(), TRI.CalleeSaveRegs.end(), [&](uint64_t R) {
          return !TRI.isFrameOrLinkReg(R) && !TRI.isVectorReg(R);
        });
    for (unsigned Mutation = 0; Mutation < 3; ++Mutation) {
      NativeContextFixture Fixture(Architecture, Register);
      if (Mutation == 0) {
        auto &Load = Fixture.Med.Blocks[0].Ops[0];
        Load.Opcode = NdOp::COPY;
        Load.Output = Fixture.Context;
        Fixture.Med.Blocks[0].Ops[1].Inputs[1] = MedVar::makeConst(7, 4);
      } else if (Mutation == 1) {
        MedOp Define;
        Define.Opcode = NdOp::COPY;
        Define.Output = Fixture.Context;
        Define.addInput(MedVar::makeConst(0, 8));
        Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin(),
                                         Define);
      } else {
        PhiNode Phi;
        Phi.Output = Fixture.Context;
        Phi.Args = {{0, MedVar::makeConst(0, 8)}};
        Fixture.Med.Blocks[0].Phis.push_back(Phi);
      }
      std::string Error;
      auto Hint = Fixture.inferContext(Error);
      ASSERT_TRUE(Hint) << Error;
      EXPECT_EQ(Hint->Parameters.size(), 1U) << Mutation;
    }
  }
}

TEST(NativeSourceHints, OmittedUnusedArgumentDoesNotShiftPhysicalRegister) {
  NativeFixture Fixture;
  Fixture.Med.Params[0].Id = -1;
  Fixture.Med.Blocks[0].Ops[0].Inputs[0] = MedVar::makeConst(3, 4);
  std::string Error;
  auto Hint = Fixture.infer(Error);
  ASSERT_TRUE(Hint) << Error;
  ASSERT_EQ(Hint->Parameters.size(), 1U);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset,
            getTargetRegInfo(Arch::AArch64).IntParamRegs[1]);
}

TEST(NativeSourceHints, ConstantFunctionNeedsNoInventedArguments) {
  NativeFixture Fixture;
  Fixture.Med.Params.clear();
  Fixture.Med.TypedParams.clear();
  Fixture.High.Params.clear();
  auto &Op = Fixture.Med.Blocks[0].Ops[0];
  Op.Opcode = NdOp::COPY;
  Op.NumInputs = 1;
  Op.Inputs[0] = MedVar::makeConst(42, 4);
  std::string Error;
  auto Hint = Fixture.infer(Error);
  ASSERT_TRUE(Hint) << Error;
  EXPECT_TRUE(Hint->Parameters.empty());
}

TEST(NativeSourceHints, RejectsIncompleteAuditAndMismatchedFunctionIdentity) {
  for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
    NativeFixture Fixture;
    if (Mutation == 0)
      Fixture.Audit.TruncatedPaths.push_back(0x1004);
    if (Mutation == 1)
      --Fixture.Audit.LiftedInstructions;
    if (Mutation == 2)
      Fixture.Audit.MedIRVerified = false;
    if (Mutation == 3)
      Fixture.High.Entry += 4;
    if (Mutation == 4)
      Fixture.Image.IsRelocatable = true;
    if (Mutation == 5)
      Fixture.Image.Segments[0].Flags = SegmentFlags::Readable;
    std::string Error;
    EXPECT_FALSE(Fixture.infer(Error)) << Mutation;
    EXPECT_FALSE(Error.empty());
  }
}

TEST(NativeSourceHints, RejectsRegisterBankTypeWidthAndDuplicateAmbiguity) {
  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    NativeFixture Fixture;
    if (Mutation == 0)
      Fixture.Med.Params[0].RegOff =
          getTargetRegInfo(Arch::AArch64).FPParamRegs[0];
    if (Mutation == 1)
      Fixture.Med.TypedParams[0].Type = NdType::makeFloat(4);
    if (Mutation == 2)
      Fixture.Med.Params[0].Size = 8;
    if (Mutation == 3)
      Fixture.Med.Params[1].RegOff = Fixture.Med.Params[0].RegOff;
    if (Mutation == 4)
      Fixture.High.ReturnType = NdType::makeInt(8);
    std::string Error;
    EXPECT_FALSE(Fixture.infer(Error)) << Mutation;
  }
}

TEST(NativeSourceHints, UnknownCallsCannotInventResultOrArgumentTypes) {
  NativeFixture Fixture;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.addInput(MedVar::makeConst(0x1020, 8));
  Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin(), Call);
  std::string Error;
  EXPECT_FALSE(Fixture.infer(Error));
  EXPECT_NE(Error.find("without a source binding"), std::string::npos);
}

TEST(NativeSourceHints, EachReturnRequiresComputedCarrierNotStaleLiveIn) {
  for (unsigned Mutation = 0; Mutation < 3; ++Mutation) {
    NativeFixture Fixture;
    auto &Op = Fixture.Med.Blocks[0].Ops[0];
    if (Mutation == 0) {
      Op.Opcode = NdOp::COPY;
      Op.NumInputs = 1;
      Op.Inputs[0] = Op.Output;
    }
    if (Mutation == 1)
      Op.Output.Size = 2;
    if (Mutation == 2) {
      MedBlock Other;
      Other.Id = 1;
      Other.Ops.push_back(Fixture.Med.Blocks[0].Ops.back());
      Fixture.Med.Blocks.push_back(Other);
    }
    std::string Error;
    EXPECT_FALSE(Fixture.infer(Error)) << Mutation;
  }
}

// The source hint consumes already verified MedIR. These graphs isolate the
// all-path carrier proof; the runtime fixture exercises real lifting and SSA.
void returnDiamond(NativeFixture &Fixture) {
  const auto Compute = Fixture.Med.Blocks[0].Ops.front();
  const auto Return = Fixture.Med.Blocks[0].Ops.back();
  Fixture.Med.Blocks.resize(4);
  for (unsigned I = 0; I < 4; ++I) {
    auto &Block = Fixture.Med.Blocks[I];
    Block.Id = I;
    Block.StartAddr = Fixture.Med.Entry + I * 16;
    Block.Ops.clear();
  }
  auto &Blocks = Fixture.Med.Blocks;
  Blocks[0].Succs = {1, 2};
  for (unsigned I : {1U, 2U}) {
    Blocks[I].Preds = {0};
    Blocks[I].Succs = {3};
    Blocks[I].Ops = {Compute};
  }
  Blocks[3].Preds = {1, 2};
  Blocks[3].Ops = {Return};
}

TEST(NativeSourceHints, ComputedReturnsMergeAcrossEveryPathAndBlockOrder) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (bool Complete : {false, true}) {
      NativeFixture Fixture(Architecture);
      // Isolate computed results from the separately proven entry carrier.
      Fixture.Med.Params[0].Id = -1;
      Fixture.Med.Blocks[0].Ops[0].Inputs[0] = MedVar::makeConst(3, 4);
      returnDiamond(Fixture);
      if (!Complete)
        Fixture.Med.Blocks[2].Ops.clear();
      const auto Blocks = Fixture.Med.Blocks;
      std::array<unsigned, 4> Order{0, 1, 2, 3};
      do {
        for (unsigned I = 0; I < 4; ++I)
          Fixture.Med.Blocks[I] = Blocks[Order[I]];
        std::string Error;
        EXPECT_EQ(bool(Fixture.infer(Error)), Complete) << Error;
        EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                  MedReturnValueEvidence::Unknown);
      } while (std::next_permutation(Order.begin(), Order.end()));
    }
  }
}

TEST(NativeSourceHints, ReturnLoopsRequireComputationOnTheirEntryPath) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (bool Seeded : {false, true}) {
      NativeFixture Fixture(Architecture);
      Fixture.Med.Params[0].Id = -1;
      Fixture.Med.Blocks[0].Ops[0].Inputs[0] = MedVar::makeConst(3, 4);
      returnDiamond(Fixture);
      auto &Blocks = Fixture.Med.Blocks;
      // Entry -> header -> body -> header, or header -> return. Computing
      // only in the body cannot prove a result on the zero-trip path.
      if (Seeded)
        Blocks[0].Ops = Blocks[2].Ops;
      Blocks[0].Succs = {1};
      Blocks[1].Ops.clear();
      Blocks[1].Preds = {0, 2};
      Blocks[1].Succs = {2, 3};
      Blocks[2].Preds = {1};
      Blocks[2].Succs = {1};
      Blocks[3].Preds = {1};
      std::string Error;
      EXPECT_EQ(bool(Fixture.infer(Error)), Seeded) << Error;
      // A disconnected cycle cannot establish its own incoming fact either.
      Blocks[0].Succs.clear();
      Blocks[1].Preds = {2};
      EXPECT_FALSE(Fixture.infer(Error));
    }
  }
}

TEST(NativeSourceHints, ReturnPathsInvalidateCallsAndPartialCarrierWrites) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      NativeFixture Fixture(Architecture);
      returnDiamond(Fixture);
      auto &Ops = Fixture.Med.Blocks[2].Ops;
      MedOp Overwrite = Ops.front();
      Overwrite.Opcode = NdOp::COPY;
      Overwrite.NumInputs = 1;
      Overwrite.Inputs[0] = MedVar::makeConst(7, 4);
      if (Mutation == 0)
        Overwrite.Output.Size = 2;
      if (Mutation == 1) {
        ++Overwrite.Output.RegOff;
        Overwrite.Output.Size = 1;
      }
      if (Mutation == 2) {
        Overwrite.Opcode = NdOp::CALL;
        Overwrite.Output = {};
        Overwrite.Inputs[0] = MedVar::makeConst(0x1020, 8);
        auto Hint = std::make_shared<SourceCallTypeHint>();
        Hint->Signature.ReturnType = NdType::makeVoid();
        std::string Error;
        ASSERT_TRUE(
            assignDarwinScalarSourceABI(Hint->Signature, Architecture, Error))
            << Error;
        Overwrite.SourceCallHint = std::move(Hint);
      }
      if (Mutation == 3)
        Overwrite.Inputs[0] = Overwrite.Output; // SSA seed, not a write.
      if (Mutation == 4) {
        Overwrite.Opcode = NdOp::SUBBYTES; // Register view, not a write.
        Overwrite.Output.Size = 1;
        Overwrite.addInput(MedVar::makeConst(0, 4));
      }
      Ops.push_back(Overwrite);
      std::string Error;
      EXPECT_EQ(bool(Fixture.infer(Error)), Mutation >= 3) << Mutation << Error;
      // A subsequent complete computation reestablishes the carrier.
      Ops.push_back(Ops.front());
      EXPECT_TRUE(Fixture.infer(Error)) << Mutation << Error;
    }
  }
}

TEST(NativeSourceHints, ReturnPathsRejectUnobservedInputAndEpilogueRestores) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeFixture Fixture(Architecture);
    returnDiamond(Fixture);
    for (unsigned I : {1U, 2U}) {
      auto &Op = Fixture.Med.Blocks[I].Ops.front();
      Op.Opcode = NdOp::COPY;
      Op.NumInputs = 1;
      Op.Inputs[0] = Op.Output;
    }
    std::string Error;
    EXPECT_FALSE(Fixture.infer(Error));
  }
  NativeFixture Fixture(Arch::X64);
  returnDiamond(Fixture);
  auto &Restore = Fixture.Med.Blocks[2].Ops.front();
  Restore.Opcode = NdOp::LOAD;
  Restore.NumInputs = 1;
  Restore.Inputs[0] = Restore.Output;
  Restore.Inputs[0].RegOff = getTargetRegInfo(Arch::X64).StackPointer;
  std::string Error;
  EXPECT_FALSE(Fixture.infer(Error));
}

TEST(NativeSourceHints, ObservedIncomingResultsKeepExactWidthsAndLocations) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (uint16_t Width : {1, 2, 4, 8}) {
      NativeFixture Fixture(Architecture);
      Fixture.Med.ReturnType = Fixture.High.ReturnType = NdType::makeInt(Width);
      for (unsigned I = 0; I < 2; ++I) {
        Fixture.Med.Params[I].Size = Width;
        Fixture.Med.TypedParams[I].Type = Fixture.High.Params[I].Type =
            NdType::makeInt(Width);
        Fixture.Med.Blocks[0].Ops[0].Inputs[I] = Fixture.Med.Params[I];
      }
      Fixture.Med.Blocks[0].Ops[0].Output.Size = Width;
      returnDiamond(Fixture);
      Fixture.Med.Blocks[2].Ops.clear();
      const auto Blocks = Fixture.Med.Blocks;
      std::array<unsigned, 4> Order{0, 1, 2, 3};
      do {
        for (unsigned I = 0; I < 4; ++I)
          Fixture.Med.Blocks[I] = Blocks[Order[I]];
        std::string Error;
        const auto Hint = Fixture.infer(Error);
        const auto &TRI = getTargetRegInfo(Architecture);
        EXPECT_EQ(bool(Hint), TRI.IntParamRegs[0] == TRI.IntReturnReg) << Error;
        if (Hint) {
          EXPECT_EQ(Hint->ReturnLocation.ValueBytes, Width);
          EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset,
                    TRI.IntReturnReg);
          EXPECT_EQ(Hint->Parameters[0].Location.ValueBytes, Width);
        }
      } while (std::next_permutation(Order.begin(), Order.end()));
    }
}

TEST(NativeSourceHints, IncomingResultsRejectUnobservedNarrowAndLaterVersions) {
  for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
    NativeFixture Fixture;
    returnDiamond(Fixture);
    Fixture.Med.Blocks[2].Ops.clear();
    auto &Use = Fixture.Med.Blocks[1].Ops[0].Inputs[0];
    if (Mutation == 0)
      Use = MedVar::makeConst(3, 4);
    if (Mutation == 1)
      Use.Size = 2;
    if (Mutation == 2) {
      Use.Kind = MedVar::Reg;
      Use.SSAVer = 1;
    }
    if (Mutation == 3) {
      Fixture.Med.Params[0].Size = 2;
      Fixture.Med.TypedParams[0].Type = Fixture.High.Params[0].Type =
          NdType::makeInt(2);
      Use = Fixture.Med.Params[0];
    }
    if (Mutation == 4) {
      // A matching parameter listed only in an unreachable component does
      // not make the actual entry register an observed input.
      Fixture.Med.Blocks[0].Succs = {2};
      Fixture.Med.Blocks[1].Preds.clear();
    }
    if (Mutation == 5 || Mutation == 6) {
      Use.Kind = MedVar::Reg;
      Use.Id = 60;
      Use.SSAVer = 0;
      // An ordinary definition or a PHI may own the first SSA version.
      // Neither is evidence that the parameter's incoming bytes were used.
      if (Mutation == 5) {
        MedOp Internal;
        Internal.Opcode = NdOp::COPY;
        Internal.Output = Use;
        Internal.addInput(MedVar::makeConst(42, 4));
        auto &Ops = Fixture.Med.Blocks[1].Ops;
        Ops.insert(Ops.begin(), Internal);
      } else {
        PhiNode Phi;
        Phi.Output = Use;
        Phi.Args = {{0, MedVar::makeConst(42, 4)}};
        Fixture.Med.Blocks[1].Phis.push_back(std::move(Phi));
      }
    }
    std::string Error;
    EXPECT_FALSE(Fixture.infer(Error)) << Mutation;
  }
}

TEST(NativeSourceHints, IncomingResultFactsInvalidateNarrowedSelfCopies) {
  for (uint16_t WriteWidth : {2, 4}) {
    NativeFixture Fixture;
    returnDiamond(Fixture);
    auto &Ops = Fixture.Med.Blocks[2].Ops;
    MedOp Copy = Ops.front();
    Copy.Opcode = NdOp::COPY;
    Copy.NumInputs = 1;
    Copy.Inputs[0] = Copy.Output;
    Copy.Output.Size = WriteWidth;
    Ops = {Copy};
    std::string Error;
    EXPECT_EQ(bool(Fixture.infer(Error)), WriteWidth == 4) << Error;
  }
}

TEST(NativeSourceHints, IncomingResultBackedgesMeetInitialAndClobberedStates) {
  for (bool Clobber : {false, true}) {
    NativeFixture Fixture;
    const auto Return = Fixture.Med.Blocks[0].Ops.back();
    MedOp Observe;
    Observe.Opcode = NdOp::COPY;
    Observe.Output.Kind = MedVar::Temp;
    Observe.Output.Id = 50;
    Observe.Output.Size = 4;
    Observe.addInput(Fixture.Med.Params[0]);
    Fixture.Med.Blocks.resize(3);
    for (unsigned I = 0; I < 3; ++I) {
      Fixture.Med.Blocks[I].Id = I;
      Fixture.Med.Blocks[I].StartAddr = Fixture.Med.Entry + I * 16;
    }
    auto &Blocks = Fixture.Med.Blocks;
    Blocks[0].Ops = {Observe};
    Blocks[0].Preds = {1};
    Blocks[0].Succs = {1, 2};
    Blocks[1].Preds = {0};
    Blocks[1].Succs = {0};
    Blocks[2].Preds = {0};
    Blocks[2].Ops = {Return};
    if (Clobber) {
      MedOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(MedVar::makeConst(0x1020, 8));
      auto Hint = std::make_shared<SourceCallTypeHint>();
      Hint->Signature.ReturnType = NdType::makeVoid();
      std::string Error;
      ASSERT_TRUE(
          assignDarwinScalarSourceABI(Hint->Signature, Arch::AArch64, Error));
      Call.SourceCallHint = std::move(Hint);
      Blocks[1].Ops = {Call};
    }
    std::string Error;
    EXPECT_EQ(bool(Fixture.infer(Error)), !Clobber) << Error;
  }
}

TEST(NativeSourceHints, ReturnProofRejectsMalformedOrUnboundedControlFlow) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 10; ++Mutation) {
      NativeFixture Fixture(Architecture);
      returnDiamond(Fixture);
      auto &Blocks = Fixture.Med.Blocks;
      if (Mutation == 0)
        Blocks[0].Succs.push_back(99);
      if (Mutation == 1)
        Blocks[0].Succs.push_back(1);
      if (Mutation == 2)
        Blocks[1].Preds.push_back(0);
      if (Mutation == 3)
        Blocks[1].Preds.clear();
      if (Mutation == 4)
        Blocks[1].Id = Blocks[0].Id;
      if (Mutation == 5)
        Blocks[0].Id = -1;
      if (Mutation == 6)
        Blocks[1].StartAddr = Fixture.Med.Entry;
      if (Mutation == 7)
        Blocks[0].StartAddr += 4;
      if (Mutation == 8)
        Blocks.resize(16385);
      if (Mutation == 9)
        Blocks[1].Ops.resize(262144, Blocks[1].Ops.front());
      std::string Error;
      EXPECT_FALSE(Fixture.infer(Error)) << Mutation;
      EXPECT_FALSE(Error.empty()) << Mutation;
    }
  }
}

TEST(NativeSourceHints, KeepsCompleteStackSlotAndRejectsPackedSlices) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeFixture Fixture(Architecture);
    auto &Parameter = Fixture.Med.Params[1];
    Parameter.RegOff = kNoParamReg;
    Parameter.Id = getTargetRegInfo(Architecture).IntParamRegs.size() + 2;
    Parameter.Size = 8;
    Fixture.Med.TypedParams[1].Type = NdType::makeInt(8);
    Fixture.Med.Blocks[0].Ops[0].Inputs[1] = Parameter;
    std::string Error;
    auto Hint = Fixture.infer(Error);
    ASSERT_TRUE(Hint) << Error;
    EXPECT_EQ(Hint->Parameters[1].Location.EntryStackOffset,
              Architecture == Arch::X64 ? 24 : 16);
    auto &Op = Fixture.Med.Blocks[0].Ops[0];
    Op.Opcode = NdOp::SUBBYTES;
    Op.Inputs[0] = Parameter;
    Op.Inputs[1] = MedVar::makeConst(4, 4);
    EXPECT_FALSE(Fixture.infer(Error));
    EXPECT_NE(Error.find("partial"), std::string::npos);
  }
}

TEST(NativeSourceHints,
     GenericScalarAllocatorDoesNotAddObjectiveCHiddenValues) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    SourceFunctionTypeHint Hint;
    Hint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Hint.ReturnType = NdType::makeFloat(8);
    Hint.Parameters = {{"integer", NdType::makeInt(4)},
                       {"floating", NdType::makeFloat(8)}};
    std::string Error;
    ASSERT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Error))
        << Error;
    const auto &TRI = getTargetRegInfo(Architecture);
    EXPECT_EQ(Hint.Parameters[0].Location.RegisterOffset, TRI.IntParamRegs[0]);
    EXPECT_EQ(Hint.Parameters[1].Location.RegisterOffset, TRI.FPParamRegs[0]);
    EXPECT_FALSE(assignDarwinObjCSourceABI(Hint, Architecture, Error));
  }
}

TEST(NativeSourceHints, InferenceSurvivesRealLowMedHighScalarPipeline) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeFixture Fixture(Architecture);
    const auto &TRI = getTargetRegInfo(Architecture);
    LowFunc Low;
    Low.Entry = Fixture.Med.Entry;
    Low.Name = "scalar_helper";
    LowBlock Block;
    Block.Id = 0;
    Block.StartAddr = Low.Entry;
    Block.EndAddr = Low.Entry + 8;
    LowOp Add;
    Add.Opcode = NdOp::INT_ADD;
    Add.Addr = Low.Entry;
    Add.Output = NdVar::reg(TRI.IntReturnReg, 4);
    Add.addInput(NdVar::reg(TRI.IntParamRegs[0], 4));
    Add.addInput(NdVar::reg(TRI.IntParamRegs[1], 4));
    Block.Ops.push_back(Add);
    LowOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Low.Entry + 4;
    Return.addInput(NdVar::reg(TRI.IntReturnReg, 4));
    Block.Ops.push_back(Return);
    Low.Blocks.push_back(Block);
    Fixture.Med =
        LowToMedConverter().convert(Low, Architecture, BinaryFormat::MachO);
    inferMedTypes(Fixture.Med, Architecture);
    Fixture.High = MedToHighConverter().convert(Fixture.Med, Architecture);
    std::string Error;
    auto Hint = Fixture.infer(Error);
    ASSERT_TRUE(Hint) << Error;
    ASSERT_EQ(Hint->Parameters.size(), 2U);
    Fixture.Med.SourceTypeHint = *Hint;
    inferMedTypes(Fixture.Med, Architecture);
    ASSERT_TRUE(Fixture.Med.SourceTypeHint);
    ASSERT_TRUE(Fixture.Med.SourceParametersBound);
    Fixture.High = MedToHighConverter().convert(Fixture.Med, Architecture);
    ASSERT_EQ(Fixture.High.Params.size(), 2U);
    EXPECT_EQ(Fixture.High.Params[0].Name, "native_arg0");
    EXPECT_EQ(Fixture.High.Params[1].Name, "native_arg1");
  }
}

void addPointerCall(NativeFixture &Fixture) {
  for (size_t Index = 0; Index < Fixture.Med.Params.size(); ++Index) {
    Fixture.Med.Params[Index].Size = 8;
    Fixture.Med.TypedParams[Index].Type = NdType::makeInt(8);
    Fixture.High.Params[Index].Type = NdType::makeInt(8);
    Fixture.Med.Blocks[0].Ops[0].Inputs[Index] = MedVar::makeConst(21, 4);
  }
  auto CallHint = std::make_shared<SourceCallTypeHint>();
  CallHint->Signature.Origin =
      SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  CallHint->Signature.ReturnType = NdType::makeVoid();
  CallHint->Signature.Parameters = {
      {"first", NdType::makePtr(NdType::makeVoid())},
      {"second", NdType::makePtr(NdType::makeVoid())}};
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(CallHint->Signature,
                                          Fixture.Image.Arch, Error))
      << Error;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.SourceCallHint = std::move(CallHint);
  Call.addInput(MedVar::makeConst(0x1020, 8));
  Call.addInput(Fixture.Med.Params[0]);
  Call.addInput(Fixture.Med.Params[1]);
  // Generic MedIR keeps the incoming SSA registers until source parameters
  // are bound in the second pipeline run. Their IDs need not match Param IDs.
  for (unsigned Index = 1; Index <= 2; ++Index) {
    Call.Inputs[Index].Kind = MedVar::Reg;
    Call.Inputs[Index].Id += 100;
  }
  Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin(), Call);
}

TEST(NativeSourceHints,
     PointerUsesFollowWholeValuesWithoutChangingMachineTypes) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Shape = 0; Shape < 4; ++Shape) {
      NativeFixture Fixture(Architecture);
      addPointerCall(Fixture);
      if (Shape == 3) {
        auto &Ops = Fixture.Med.Blocks[0].Ops;
        MedOp SelfCopy;
        SelfCopy.Opcode = NdOp::COPY;
        SelfCopy.Output = Ops[0].Inputs[1];
        SelfCopy.addInput(SelfCopy.Output);
        Ops.insert(Ops.begin(), SelfCopy);
      } else if (Shape) {
        MedOp Copy;
        Copy.Opcode = NdOp::COPY;
        Copy.Output.Kind = MedVar::Temp;
        Copy.Output.Id = 30;
        Copy.Output.SSAVer = 1;
        Copy.Output.Size = 8;
        Copy.addInput(Fixture.Med.Params[0]);
        auto &Entry = Fixture.Med.Blocks[0];
        Entry.Ops[0].Inputs[1] = Copy.Output;
        if (Shape == 1) {
          Entry.Ops.insert(Entry.Ops.begin(), Copy);
        } else {
          // A loop PHI retains the exact incoming value through its backedge.
          MedBlock Exit = std::move(Entry);
          Exit.Id = 1;
          Exit.Preds = {2};
          MedBlock Loop;
          Loop.Id = 2;
          Loop.Preds = {0, 2};
          Loop.Succs = {2, 1};
          PhiNode Phi;
          Phi.Output = Copy.Output;
          ++Phi.Output.Id;
          Phi.Args = {{0, Copy.Output}, {2, Phi.Output}};
          Loop.Phis.push_back(Phi);
          Exit.Ops[0].Inputs[1] = Phi.Output;
          Entry = {};
          Entry.Id = 0;
          Entry.Succs = {2};
          Entry.Ops.push_back(Copy);
          // Deliberately keep the consumer before the producer in block order.
          Fixture.Med.Blocks.push_back(std::move(Exit));
          Fixture.Med.Blocks.push_back(std::move(Loop));
        }
      }
      std::string Error;
      auto Hint = Fixture.infer(Error);
      ASSERT_TRUE(Hint) << Error;
      ASSERT_EQ(Hint->Parameters.size(), 2U);
      for (size_t Index = 0; Index < 2; ++Index) {
        EXPECT_EQ(Hint->Parameters[Index].Type->Kind, NdTypeKind::Ptr);
        EXPECT_EQ(Hint->Parameters[Index].Location.ValueBytes, 8U);
        EXPECT_EQ(Hint->Parameters[Index].Location.RegisterOffset,
                  getTargetRegInfo(Architecture).IntParamRegs[Index]);
        EXPECT_EQ(Fixture.Med.TypedParams[Index].Type->Kind, NdTypeKind::Int);
        EXPECT_EQ(Fixture.High.Params[Index].Type->Kind, NdTypeKind::Int);
      }
      EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                MedReturnValueEvidence::Unknown);
    }
  }
}

TEST(NativeSourceHints, ConflictingOrPartialUsesCannotInventPointerParameters) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      NativeFixture Fixture(Architecture);
      addPointerCall(Fixture);
      auto &Ops = Fixture.Med.Blocks[0].Ops;
      if (Mutation == 0) {
        Ops[1].Inputs[0] = Fixture.Med.Params[0];
      } else if (Mutation == 1) {
        MedOp Copy;
        Copy.Opcode = NdOp::COPY;
        Copy.Output.Kind = MedVar::Temp;
        Copy.Output.Id = 30;
        Copy.Output.Size = 4;
        Copy.addInput(Fixture.Med.Params[0]);
        Ops.insert(Ops.begin(), Copy);
      } else if (Mutation == 2) {
        auto Hint =
            std::make_shared<SourceCallTypeHint>(*Ops[0].SourceCallHint);
        Hint->Signature.Parameters[0].Type = NdType::makeInt(8);
        auto ScalarCall = Ops[0];
        ScalarCall.SourceCallHint = std::move(Hint);
        Ops.insert(Ops.begin(), ScalarCall);
      } else if (Mutation == 3) {
        // A later register version is not the incoming parameter.
        Ops[0].Inputs[1].Kind = MedVar::Reg;
        ++Ops[0].Inputs[1].SSAVer;
      } else {
        Ops[0].Inputs[1].Size = 4;
      }
      std::string Error;
      auto Hint = Fixture.infer(Error);
      ASSERT_TRUE(Hint) << Error;
      EXPECT_EQ(Hint->Parameters[0].Type->Kind, NdTypeKind::Int) << Mutation;
      EXPECT_EQ(Hint->Parameters[1].Type->Kind, NdTypeKind::Ptr) << Mutation;
    }
  }
}

TEST(NativeSourceHints, CallClobbersDoNotBecomeIncomingPointerEvidence) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeFixture Fixture(Architecture);
    addPointerCall(Fixture);
    // The first implicit definition of a register can still have version zero.
    // Passing that unavailable result to a pointer consumer says nothing about
    // the original entry value in the same physical register.
    MedCallClobber Clobber;
    Clobber.Value = Fixture.Med.Blocks[0].Ops[0].Inputs[1];
    Fixture.Med.CallClobbers.push_back(Clobber);
    const auto Pointers = inferMedSourcePointerParameters(Fixture.Med);
    ASSERT_EQ(Pointers.size(), 2U);
    EXPECT_FALSE(Pointers[0]);
    EXPECT_TRUE(Pointers[1]);
  }
}

TEST(NativeSourceHints, ImportedPointerForwardersShareBothSourceSignatures) {
  class Header : public NullDebugContext {
  public:
    bool ExplicitEntry = false;
    std::optional<FunctionSym>
    resolvePrototype(llvm::StringRef Name) const override {
      if (Name != "consume_pointer")
        return std::nullopt;
      FunctionSym Function;
      Function.Name = Name.str();
      Function.ReturnType = NdType::makeVoid();
      Function.Params = {{"object", NdType::makePtr(NdType::makeVoid())},
                         {"tag", NdType::makeInt(4, false)}};
      Function.PrototypeFromHeader = true;
      Function.PrototypeLibrary = "test-header";
      return Function;
    }
    std::optional<FunctionSym> resolveFunction(va_t Address) const override {
      if (Address == 0x1000 && ExplicitEntry) {
        FunctionSym Entry;
        Entry.Addr = Address;
        Entry.Name = "forward_pointer";
        Entry.ReturnType = NdType::makeVoid();
        Entry.Params = {{"arg0", NdType::makeInt(8)}};
        Entry.PrototypeFromHeader = true;
        Entry.PrototypeLibrary = "test-header";
        return Entry;
      }
      auto Function = Address == 0x2000 ? resolvePrototype("consume_pointer")
                                        : std::nullopt;
      if (Function)
        Function->Addr = Address;
      return Function;
    }
  } Debug;
  for (auto Architecture : {Arch::X64, Arch::AArch64})
    for (auto Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      SCOPED_TRACE(unsigned(Architecture));
      SCOPED_TRACE(unsigned(Format));
      BinaryImage Image;
      Image.Arch = Architecture;
      Image.Format = Format;
      Image.Bits = Bitness::Bits64;
      Image.Entry = Image.Base = 0x1000;
      Segment Text;
      Text.VA = Image.Entry;
      Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
      if (Architecture == Arch::X64) {
        // mov edx/esi,42; jmp [0x2000]. This wrapper supplies one argument;
        // it is not an import veneer with an interchangeable source identity.
        Text.Data = {uint8_t(Format == BinaryFormat::COFF ? 0xba : 0xbe),
                     42,
                     0,
                     0,
                     0,
                     0xff,
                     0x25,
                     0xf5,
                     0x0f,
                     0,
                     0};
      } else {
        // mov x1,42; adrp x16,0x2000; ldr x16,[x16]; br x16
        Text.Data = {0x41, 5, 0x80, 0xd2, 0x10, 0, 0,    0xb0,
                     0x10, 2, 0x40, 0xf9, 0,    2, 0x1f, 0xd6};
      }
      Text.Size = Text.FileSz = Text.Data.size();
      Image.Segments.push_back(std::move(Text));
      Segment Slot;
      Slot.VA = 0x2000;
      Slot.Size = Slot.FileSz = 8;
      Slot.Data.resize(8);
      Slot.Flags = SegmentFlags::Readable;
      Image.Segments.push_back(std::move(Slot));
      Import Import;
      Import.Name = "consume_pointer";
      Import.Module = "fixture";
      Import.IATAddr = 0x2000;
      Image.Imports.push_back(std::move(Import));
      ASSERT_TRUE(Image.recordImportStorageSlot(
          0x2000, "consume_pointer", 0, ImportStorageEvidence::LoaderBind));
      auto FunctionSymbol = Symbol::makeFunc(0x1000);
      FunctionSymbol.Name = "forward_pointer";
      Image.Symbols.push_back(std::move(FunctionSymbol));
      for (bool Explicit : {false, true})
        for (bool LLVM : {false, true}) {
          Debug.ExplicitEntry = Explicit;
          SCOPED_TRACE(Explicit);
          SCOPED_TRACE(LLVM);
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.EmitDumpOutput = false;
          Options.OnlyFunctionEntries = {0x1000};
          Options.LiftMode = Options.SourceProjection = LLVM;
          auto Result = Pipeline().run(Image, Context, Options, &Debug);
          ASSERT_TRUE(Result.Success) << Result.Error;
          ASSERT_EQ(Result.MedFuncs.size(), 1U);
          ASSERT_EQ(Result.MedFuncs[0].TypedParams.size(), 1U);
          EXPECT_EQ(Result.MedFuncs[0].TypedParams[0].Type->Kind,
                    Explicit ? NdTypeKind::Int : NdTypeKind::Ptr);
          CEmitterOptions COptions;
          COptions.TheArch = Architecture;
          COptions.Format = Format;
          COptions.Image = &Image;
          std::string Source;
          llvm::raw_string_ostream Out(Source);
          if (LLVM) {
            ASSERT_TRUE(Result.LlvmModule);
            ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions,
                                            &Debug, &Image));
          } else {
            ASSERT_TRUE(
                HighCEmitter().emit(Result.HighFuncs, Out, COptions, &Debug));
          }
          EXPECT_EQ(Source.find("forward_pointer(void* arg0)") !=
                        std::string::npos,
                    !Explicit)
              << Source;
        }
    }
}

TEST(NativeSourceHints, AmbiguousValuesAndUnboundCallsDoNotSupplyPointerFacts) {
  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    NativeFixture Fixture;
    addPointerCall(Fixture);
    auto &Ops = Fixture.Med.Blocks[0].Ops;
    if (Mutation == 0) {
      Ops[0].SourceCallHint.reset();
    } else if (Mutation == 1) {
      auto Hint = std::make_shared<SourceCallTypeHint>(*Ops[0].SourceCallHint);
      Hint->Signature.Parameters[0].Location.ValueBytes = 4;
      Ops[0].SourceCallHint = std::move(Hint);
    } else if (Mutation == 2) {
      const auto Duplicate = Ops[1];
      Ops.insert(Ops.begin(), Duplicate);
    } else if (Mutation == 3) {
      Ops[0].Inputs[1] = Fixture.Med.Params[0];
      Ops[0].Inputs[1].RegOff = Fixture.Med.Params[1].RegOff;
    } else {
      PhiNode Phi;
      Phi.Output = Ops[0].Inputs[1];
      Phi.Args = {{0, MedVar::makeConst(7, 8)}};
      Fixture.Med.Blocks[0].Phis.push_back(Phi);
    }
    std::string Error;
    auto Hint = Fixture.infer(Error);
    if (Mutation < 2) {
      EXPECT_FALSE(Hint);
    } else {
      ASSERT_TRUE(Hint) << Error;
      EXPECT_EQ(Hint->Parameters[0].Type->Kind, NdTypeKind::Int);
      EXPECT_EQ(Hint->Parameters[1].Type->Kind, NdTypeKind::Int);
    }
  }
}
} // namespace

namespace {
struct ObjCContextEffectFixture : immutable_native_call_test::Fixture {
  static constexpr va_t Callee = immutable_native_call_test::Target;
  static constexpr va_t Stub = 0x1280, Counter = 0x4000;
  static constexpr va_t SiteAddress = immutable_native_call_test::Entry + 32;
  ObjCContextEffectFixture(bool Indirect = false, bool Profile = true) {
    using namespace immutable_native_call_test;
    using runtime_function_address_test::Slot;
    Image.ImportPtrSlots[Slot] = "_objc_msgSend";
    Image.DyldBindSlots[Slot] = {"_objc_msgSend", 0, "/usr/lib/libobjc.A.dylib",
                                 false};
    Image.DynInfo.NeededLibs = {
        "/System/Library/Frameworks/UIKit.framework/UIKit",
        "/usr/lib/libobjc.A.dylib"};
    Image.Segments[1].Size = Image.Segments[1].FileSz = 16;
    Image.Segments[1].Data.resize(16);
    Image.Sections[0].Size = Image.Sections[0].FileSz = 16;
    Image.ObjCSourceReferences[Slot + 8] = {ObjCSourceReference::Kind::Selector,
                                            Slot + 8,
                                            8,
                                            "setImageEdgeInsets:",
                                            {}};
    Image.Sections[1].Size = Image.Sections[1].FileSz = 0x280;
    Section Stubs;
    Stubs.Name = "__objc_stubs";
    Stubs.VA = Stub;
    Stubs.FileOff = Stub - Entry;
    Stubs.Size = Stubs.FileSz = 0x80;
    Stubs.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Stubs.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    Image.Sections.push_back(Stubs);
    Segment Data;
    Data.VA = Counter;
    Data.Size = Data.FileSz = 8;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(8);
    Image.Segments.push_back(Data);
    Section Count;
    Count.VA = Counter;
    Count.Name = "__llvm_prf_cnts";
    Count.Size = Count.FileSz = 8;
    Count.Flags = Data.Flags;
    Image.Sections.push_back(Count);
    const uint32_t StubWords[] = {0xb0000001, 0xf9400421, 0xb0000010,
                                  0xf9400210, 0xd61f0200};
    for (size_t I = 0; I < std::size(StubWords); ++I)
      word((Stub - Entry) / 4 + I, StubWords[I]);
    const uint32_t ProfileWords[] = {0xf9400280, 0xf0000008, 0xf9400109,
                                     0x91000529, 0xf9000109, 0x1400001b};
    for (size_t I = 0; I < std::size(ProfileWords); ++I)
      word((Callee - Entry) / 4 + I, ProfileWords[I]);
    if (!Profile)
      word((Callee - Entry) / 4 + 1, 0x1400001f);
    const uint32_t Caller[] = {0xa9bc53f3, 0xa9017bfd, 0x910043fd, 0xf90013e0,
                               0x910083f4, 0xd0000013, 0x91008273, 0xf9400670,
                               0xd63f0200, 0xa9417bfd, 0xa8c453f3, 0xd65f03c0};
    for (size_t I = 0; I < std::size(Caller); ++I)
      word(I, Caller[I]);
    if (!Indirect)
      word((SiteAddress - Entry) / 4,
           0x94000000 | ((Callee - SiteAddress) / 4));
    Image.Symbols[0].Size = sizeof(Caller);
    Image.Symbols.back().Size = Profile ? 24 : 8;
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters.clear();
    for (unsigned I = 0; I < 4; ++I)
      Signature.Parameters.push_back(
          {"lane" + std::to_string(I), NdType::makeFloat(8)});
    Signature.Parameters.push_back({"context", NdType::makeInt(8)});
    std::string Error;
    EXPECT_TRUE(assignDarwinScalarSourceABI(Signature, Arch::AArch64, Error));
    Signature.Parameters.back().Location.RegisterOffset = a64reg::X20;
    run();
  }
  SourceCallOccurrenceKey site(bool Indirect = false) const {
    return {SiteAddress, 1, Indirect ? NdOp::INDIR_CALL : NdOp::CALL,
            Indirect ? std::nullopt : std::optional<va_t>(Callee)};
  }
  std::optional<SourceFunctionTypeHint> infer(std::string &Error) {
    const auto Audit =
        std::find_if(Result.FunctionAudits.begin(), Result.FunctionAudits.end(),
                     [&](const auto &A) { return A.Entry == low()->Entry; });
    if (Audit == Result.FunctionAudits.end())
      return std::nullopt;
    return inferNativeSourceTypeHint(Image, *med(), *high(), *Audit, Error,
                                     low());
  }
};
} // namespace

TEST(NativeSourceHints, ContextBorrowAuthenticatesBodyDispatchAndABI) {
  for (bool Indirect : {false, true})
    for (bool Profile : {false, true}) {
      ObjCContextEffectFixture F(Indirect, Profile);
      ASSERT_TRUE(F.Result.Success) << F.Result.Error;
      ASSERT_TRUE(F.low());
      ASSERT_TRUE(isObjCContextProjection(F.Image, F.Callee));
      const auto Effects =
          objcContextProjectionEffects(F.Image, F.Callee, F.Signature);
      ASSERT_TRUE(Effects);
      EXPECT_EQ(Effects->ReadOnlyFrameParameters.at(4), 8U);
      EXPECT_TRUE(Effects->WritableFrameParameters.empty());
      EXPECT_FALSE(Effects->ReturnFrameOrExternal);
      const std::map<va_t, SourceFunctionTypeHint> Callees{
          {F.Callee, F.Signature}};
      EXPECT_EQ(objcContextCallEffects(F.Image, *F.low(), F.site(Indirect),
                                       F.Signature, &Callees),
                Effects);
      std::string Error;
      const auto Hint = F.infer(Error);
      ASSERT_TRUE(Hint) << Error;
      EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
    }
}

TEST(NativeSourceHints, ContextBorrowRejectsStaleBodyStorageAndDeclarations) {
  ObjCContextEffectFixture F;
  ASSERT_TRUE(objcContextProjectionEffects(F.Image, F.Callee, F.Signature));
  for (unsigned Mutation = 0; Mutation < 35; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = F.Image;
    auto ABI = F.Signature;
    auto Word = [&](unsigned I, uint32_t W) {
      llvm::support::endian::write32le(
          Image.Segments[0].Data.data() + F.Callee - Image.Entry + I * 4, W);
    };
    switch (Mutation) {
    case 0:
      Word(0, 0xb9400280);
      break; // Partial context load.
    case 1:
      Word(0, 0xf9400680);
      break; // Offset beyond proved first word.
    case 2:
      Word(0, 0xaa1403e0);
      break; // Escaping context, not loaded object.
    case 3:
      Word(0, 0xf9400260);
      break; // Different context carrier.
    case 4:
      Word(0, 0xf94003e0);
      break; // Incoming stack, not context.
    case 5:
      Word(1, 0xf0000009);
      break; // Wrong counter base.
    case 6:
      Word(2, 0xf9400289);
      break; // Context-based counter read.
    case 7:
      Word(3, 0x91000929);
      break; // Different counter operation.
    case 8:
      Word(4, 0xf9000509);
      break; // Different write range.
    case 9:
      Word(4, 0xf9000114);
      break; // Context escapes to image storage.
    case 10:
      Word(5, 0x9400001b);
      break; // Not a tail branch.
    case 11:
      Word(5, 0x1400001c);
      break; // Interior selector entry.
    case 12:
      Image.Segments.back().ReadOnlyAfterRelocations = true;
      break;
    case 13:
      Image.Sections.back().FileSz = 7;
      break;
    case 14:
      Image.Segments.push_back(Image.Segments.back());
      break;
    case 15:
      Image.Sections.back().Type = llvm::MachO::S_ZEROFILL;
      break;
    case 16:
      Image.Symbols.push_back({"interior", F.Callee + 4, 4, true});
      break;
    case 17:
      Image.CodePtrRelocSlots.insert(F.Callee + 8);
      break;
    case 18:
      Image.Segments[0].Flags =
          Image.Segments[0].Flags | SegmentFlags::Writable;
      break;
    case 19:
      Image.DyldBindSlots[0x2000].WeakImport = true;
      break;
    case 20:
      Image.DyldBindSlots[0x2000].Module = "/usr/lib/other.dylib";
      break;
    case 21:
      Image.DyldBindSlots[0x2000].Addend = 8;
      break;
    case 22:
      Image.ObjCSourceReferences[0x2008].Name = "missingDeclaration:";
      break;
    case 23:
      Image.DynInfo.NeededLibs.clear();
      break;
    case 24:
      ABI.ReturnType = NdType::makeInt(8);
      break;
    case 25:
      ABI.Parameters.back().Type = NdType::makeInt(4);
      ABI.Parameters.back().Location.ValueBytes = 4;
      break;
    case 26:
      ABI.Parameters.back().Location.RegisterOffset = a64reg::X19;
      break;
    case 27:
      ABI.Parameters[0].Type = NdType::makeFloat(4);
      ABI.Parameters[0].Location.ValueBytes = 4;
      break;
    case 28:
      ABI.Parameters.erase(ABI.Parameters.begin());
      break;
    case 29:
      ABI.Parameters[0].Location.RegisterOffset =
          ABI.Parameters[1].Location.RegisterOffset;
      break;
    case 30:
      ABI.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
      break;
    case 31:
      Image.Arch = Arch::X64;
      break;
    case 32:
      Image.IsRelocatable = true;
      break;
    case 33:
      Image.MachOChainedFixupsAmbiguous = true;
      break;
    case 34: {
      auto Hint = parseObjCMethodEncoding("setImageEdgeInsets:", "v24@0:8q16");
      ASSERT_TRUE(Hint);
      std::string Error;
      ASSERT_TRUE(assignDarwinObjCSourceABI(*Hint, Arch::AArch64, Error));
      ObjCMethod Method;
      Method.ClassName = "Conflict";
      Method.Selector = "setImageEdgeInsets:";
      Method.Implementation = 0x1400;
      Method.TypeHint = *Hint;
      Image.ObjCMethods.push_back(Method);
      break;
    }
    }
    EXPECT_FALSE(objcContextProjectionEffects(Image, F.Callee, ABI));
  }
  auto Renamed = F.Image;
  for (auto &S : Renamed.Symbols)
    S.Name = "unrelated";
  EXPECT_TRUE(objcContextProjectionEffects(Renamed, F.Callee, F.Signature));
}

TEST(NativeSourceHints, ContextBorrowRechecksOriginalCallAndCallerFrame) {
  for (bool Indirect : {false, true}) {
    ObjCContextEffectFixture F(Indirect);
    const std::map<va_t, SourceFunctionTypeHint> Callees{
        {F.Callee, F.Signature}};
    for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
      SCOPED_TRACE(Indirect);
      SCOPED_TRACE(Mutation);
      auto Image = F.Image;
      auto Low = *F.low();
      auto Site = F.site(Indirect);
      if (Mutation == 0)
        ++Site.Sequence;
      if (Mutation == 1)
        Site.Instruction += 4;
      if (Mutation == 2)
        Low.DecodedInstructionCount = 0;
      if (Mutation == 3)
        Low.Blocks[0].InstructionBoundaries.clear();
      if (Mutation == 4) {
        for (auto &B : Low.Blocks)
          for (auto &Op : B.Ops)
            if (Op.Addr == Site.Instruction && Op.Seq == 1)
              Op.Inputs[0] = NdVar::reg(a64reg::X17, 8);
      }
      if (Mutation == 5)
        llvm::support::endian::write32le(Image.Segments[0].Data.data() +
                                             Site.Instruction - Image.Entry,
                                         0xd503201f);
      if (Mutation == 6)
        Site.StaticTarget = F.Callee + 4;
      EXPECT_FALSE(
          objcContextCallEffects(Image, Low, Site, F.Signature, &Callees));
    }
    // The borrow certificate does not allow placing any private-frame bytes
    // into the object word that the callee forwards to external dispatch.
    auto *Low = const_cast<LowFunc *>(F.low());
    auto &Ops = Low->Blocks[0].Ops;
    auto Store = std::find_if(Ops.begin(), Ops.end(), [](const auto &Op) {
      return Op.Addr == 0x100c && Op.Opcode == NdOp::STORE;
    });
    ASSERT_NE(Store, Ops.end());
    Store->Inputs[1] = NdVar::reg(a64reg::SP, 8);
    std::string Error;
    EXPECT_FALSE(F.infer(Error)) << Error;
  }
}

TEST(NativeSourceHints, ContextBorrowInferenceRejectsChangedCallEvidence) {
  for (bool Indirect : {false, true})
    for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
      SCOPED_TRACE(Indirect);
      SCOPED_TRACE(Mutation);
      ObjCContextEffectFixture F(Indirect);
      auto &Ops = F.med()->Blocks[0].Ops;
      auto Call = std::find_if(Ops.begin(), Ops.end(), [&](const auto &Op) {
        return Op.Addr == F.SiteAddress &&
               (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL);
      });
      ASSERT_NE(Call, Ops.end());
      ASSERT_TRUE(Call->SourceCallHint);
      auto Mutable =
          std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
      Call->SourceCallHint = Mutable;
      auto &Binding = *Mutable;
      if (Mutation == 0)
        ++Call->OriginSeq;
      if (Mutation == 1)
        Call->PreservesCallerSaved = true;
      if (Mutation == 2)
        Binding.DoesNotReturn = Call->DoesNotReturn = true;
      if (Mutation == 3) {
        Binding.Signature.Parameters.back().Type = NdType::makeInt(4);
        Binding.Signature.Parameters.back().Location.ValueBytes = 4;
        Call->Inputs[Call->NumInputs - 1].Size = 4;
      }
      if (Mutation == 4) {
        Binding.Signature.ReturnType = NdType::makeInt(8);
        Binding.Signature.ReturnLocation = {
            SourceABICarrierKind::IntegerRegister, a64reg::X0, 0, 8};
      }
      if (Mutation == 5)
        Binding.Signature.Parameters.back().Location.RegisterOffset =
            a64reg::X19;
      if (Mutation == 6)
        --Call->NumInputs;
      std::string Error;
      EXPECT_FALSE(F.infer(Error)) << Error;
    }
}

namespace {
struct SwiftAccessEffectFixture : immutable_native_call_test::Fixture {
  static constexpr va_t BeginSite = 0x1018, EndSite = 0x1020;
  static constexpr va_t BeginVeneer = 0x1100, EndVeneer = 0x1110;
  static constexpr va_t BeginSlot = 0x2000, EndSlot = 0x2008;
  SwiftAccessEffectFixture(uint64_t Flags = 0, bool ScalarResult = false) {
    Image.Segments[1].Size = Image.Segments[1].FileSz = 16;
    Image.Segments[1].Data.resize(16);
    Image.Sections[0].Size = Image.Sections[0].FileSz = 16;
    for (const auto &[Slot, Name] : {std::pair{BeginSlot, "_swift_beginAccess"},
                                     std::pair{EndSlot, "_swift_endAccess"}}) {
      Image.ImportPtrSlots[Slot] = Name;
      Image.DyldBindSlots[Slot] = {Name, 0, "/usr/lib/swift/libswiftCore.dylib",
                                   false};
    }
    const uint32_t Words[] = {0xa9bc7bfd,
                              0x910003fd,
                              0xaa1403e0,
                              0x910043e1,
                              uint32_t(0xd2800002 | (Flags << 5)),
                              0xd2800003,
                              0x9400003a,
                              0x910043e0,
                              0x9400003c,
                              ScalarResult ? 0xd28000e0u : 0xd503201fu,
                              0xa8c47bfd,
                              0xd65f03c0};
    for (unsigned I = 0; I < std::size(Words); ++I)
      word(I, Words[I]);
    word((EndVeneer - 0x1000) / 4, 0xb0000010);
    word((EndVeneer - 0x1000) / 4 + 1, 0xf9400610);
    word((EndVeneer - 0x1000) / 4 + 2, 0xd61f0200);
    Image.Symbols[0].Size = sizeof(Words);
    Image.Symbols[1].Name = "_begin_veneer";
    Image.Symbols.push_back({"_end_veneer", EndVeneer, 12, true});
    run();
  }
  SourceCallOccurrenceKey site(bool Begin) const {
    return {Begin ? BeginSite : EndSite, 1, NdOp::CALL,
            Begin ? BeginVeneer : EndVeneer};
  }
  std::optional<SourceFrameEffects> effect(bool Begin) const {
    const auto Binding =
        swiftRuntimeSourceCallHint(Image, Begin ? BeginSlot : EndSlot);
    return Binding && low()
               ? swiftAccessCallEffects(Image, *low(), site(Begin), *Binding)
               : std::nullopt;
  }
  std::optional<SourceFunctionTypeHint> infer(std::string &Error) {
    const auto Audit =
        std::find_if(Result.FunctionAudits.begin(), Result.FunctionAudits.end(),
                     [&](const auto &A) { return A.Entry == 0x1000; });
    return Audit != Result.FunctionAudits.end() && med() && high() && low()
               ? inferNativeSourceTypeHint(Image, *med(), *high(), *Audit,
                                           Error, low())
               : std::nullopt;
  }
};
} // namespace

TEST(NativeSourceHints,
     SwiftAccessEffectsAuthenticateOriginalCallsAndUntrackedScratch) {
  for (unsigned Flags : {0, 1})
    for (bool Scalar : {false, true}) {
      SwiftAccessEffectFixture F(Flags, Scalar);
      ASSERT_TRUE(F.Result.Success) << F.Result.Error;
      ASSERT_TRUE(F.low());
      const auto Begin = F.effect(true), End = F.effect(false);
      ASSERT_TRUE(Begin);
      ASSERT_TRUE(End);
      ASSERT_TRUE(Begin->Scratch);
      ASSERT_TRUE(End->Scratch);
      EXPECT_EQ(Begin->WritableFrameParameters.at(1), 24U);
      EXPECT_EQ(End->ReadOnlyFrameParameters.at(0), 24U);
      EXPECT_EQ(Begin->Scratch->Condition->Values,
                (std::set<uint64_t>{0, 1, 32, 33}));
      EXPECT_EQ(Begin->Scratch->RetainedValues, (std::set<uint64_t>{32, 33}));
      EXPECT_TRUE(End->Scratch->RetainedValues.empty());
      std::string Error;
      EXPECT_TRUE(F.infer(Error)) << Error;
    }
}

TEST(NativeSourceHints, SwiftTrackedAccessRequiresBalancedMachineLifetime) {
  for (unsigned Flags : {0x20, 0x21})
    for (bool Scalar : {false, true}) {
      SwiftAccessEffectFixture F(Flags, Scalar);
      std::string Error;
      ASSERT_TRUE(F.infer(Error)) << Flags << ": " << Error;
      // Keeping the result and return ABI unchanged cannot excuse a missing
      // end: the runtime would retain this soon-to-expire frame in TLS.
      F.word(8, 0xd503201f);
      F.run();
      EXPECT_FALSE(F.infer(Error));
    }
}

TEST(NativeSourceHints, SwiftTrackedAccessAuthenticatesNestedMachineCalls) {
  for (unsigned Flags : {32, 33})
    for (bool Lifo : {false, true})
      for (unsigned Case = 0; Case < 5; ++Case) {
        SCOPED_TRACE(Case);
        SwiftAccessEffectFixture F(Flags, true);
        // Two records in a 96-byte frame, at +16 and +48. Their accessed
        // objects are x20 and x20+8. Ending the older record first lets the
        // runtime update the younger record's opaque link.
        const uint32_t Words[] = {0xa9ba7bfd,
                                  0x910003fd,
                                  0xaa1403e0,
                                  0x910043e1,
                                  uint32_t(0xd2800002 | (Flags << 5)),
                                  0xd2800003,
                                  0x9400003a,
                                  0x91002280,
                                  0x9100c3e1,
                                  uint32_t(0xd2800002 | (Flags << 5)),
                                  0xd2800003,
                                  0x94000035,
                                  Lifo ? 0x9100c3e0u : 0x910043e0u,
                                  0x94000037,
                                  Lifo ? 0x910043e0u : 0x9100c3e0u,
                                  0x94000035,
                                  0xd28000e0,
                                  0xa8c67bfd,
                                  0xd65f03c0};
        for (unsigned I = 0; I < std::size(Words); ++I)
          F.word(I, Words[I]);
        F.Image.Symbols[0].Size = sizeof(Words);
        if (Case == 1)
          F.word(13, 0xd503201f); // omit first end
        if (Case == 2)
          F.word(15, 0xd503201f); // omit last end
        if (Case == 3)
          F.word(8, 0x910043e1); // second begin overwrites first
        if (Case == 4)
          F.word(14, Words[12]); // end same record twice
        F.run();
        std::string Error;
        EXPECT_EQ(bool(F.infer(Error)), Case == 0) << Error;
      }
}

TEST(NativeSourceHints, SwiftAccessEffectsRejectUnknownAndExpiredScratch) {
  for (bool Scalar : {false, true}) {
    for (unsigned Flags : {2, 0x22, 0x23, 0x100, 0xffff}) {
      SwiftAccessEffectFixture F(Flags, Scalar);
      std::string Error;
      EXPECT_FALSE(F.infer(Error)) << Flags;
    }
    for (unsigned Case = 0; Case < 8; ++Case) {
      SCOPED_TRACE(Case);
      SwiftAccessEffectFixture F(0, Scalar);
      switch (Case) {
      case 0:
        F.word(4, 0xaa1503e2);
        break; // unknown flags from x21
      case 1:
        F.word(6, 0xd503201f);
        break; // no initialization
      case 2:
        F.word(5, 0x910003e1);
        break; // scratch overlaps fp/lr
      case 3:
        F.word(7, 0x910023e0);
        break; // wrong record start
      case 4:
        F.word(7, 0x110043e0);
        break; // truncated frame pointer
      case 5:
        F.word(7, 0xf9000bff);
        F.word(8, 0x910043e0);
        F.word(9, 0x9400003b);
        break; // overwrite then end correct address
      case 6:
        F.word(7, 0x9100e3e0);
        break; // out-of-bounds record
      case 7:
        F.word(3, 0x9100c3e1);
        break; // begins over frame end
      }
      F.run();
      std::string Error;
      EXPECT_FALSE(F.infer(Error)) << Error;
    }
  }
}

TEST(NativeSourceHints, SwiftAccessEffectsRejectChangedOccurrenceImportAndABI) {
  SwiftAccessEffectFixture F;
  ASSERT_TRUE(F.low());
  for (bool Begin : {false, true}) {
    const auto Original =
        swiftRuntimeSourceCallHint(F.Image, Begin ? F.BeginSlot : F.EndSlot);
    ASSERT_TRUE(Original);
    for (unsigned Case = 0; Case < 24; ++Case) {
      SCOPED_TRACE(Case);
      auto Image = F.Image;
      auto Low = *F.low();
      auto Binding = *Original;
      auto Site = F.site(Begin);
      auto &Import = Image.DyldBindSlots.at(Binding.TargetAddress);
      auto &Block = Low.Blocks.front();
      auto Call = std::find_if(
          Block.Ops.begin(), Block.Ops.end(), [&](const LowOp &Op) {
            return Op.Addr == Site.Instruction && Op.Opcode == NdOp::CALL;
          });
      ASSERT_NE(Call, Block.Ops.end());
      switch (Case) {
      case 0:
        Import.WeakImport = true;
        break;
      case 1:
        Import.Module = "/tmp/impostor.dylib";
        break;
      case 2:
        Import.Addend = 8;
        break;
      case 3:
        Image.ImportPtrSlots[Binding.TargetAddress] = "_swift_release";
        break;
      case 4:
        Image.Segments[1].Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
        break;
      case 5:
        Image.ConflictingImportStorageSlots.insert(Binding.TargetAddress);
        break;
      case 6:
        llvm::support::endian::write32le(Image.Segments[0].Data.data() +
                                             Site.Instruction - 0x1000,
                                         0xd63f0200);
        break;
      case 7:
        Call->Seq = 3;
        break;
      case 8:
        Call->Inputs[0] = NdVar::cst(0x1200, 8);
        break;
      case 9:
        Call->NumInputs = 2;
        break;
      case 10:
        Call->Opcode = NdOp::INDIR_CALL;
        break;
      case 11:
        Binding.Signature.Parameters[0].Location.RegisterOffset += 8;
        break;
      case 12:
        Binding.Signature.ReturnType = NdType::makeInt(8);
        break;
      case 13:
        Binding.Signature.HasExplicitABI = false;
        break;
      case 14:
        Binding.Signature.Origin =
            SourceFunctionTypeHint::OriginKind::NativeAnalysis;
        break;
      case 15:
        Binding.Signature.Convention =
            SourceFunctionTypeHint::ConventionKind::Swift;
        break;
      case 16:
        Binding.WeakImport = true;
        break;
      case 17:
        Binding.TargetName += "_forged";
        break;
      case 18:
        Binding.TargetAddress += 8;
        break;
      case 19:
        Binding.DoesNotReturn = true;
        break;
      case 20:
        Low.DecodedInstructionCount = 0;
        break;
      case 21:
        Block.InstructionBoundaries.clear();
        break;
      case 22:
        Block.Ops.push_back(*Call);
        break;
      case 23:
        Site.Sequence = 0;
        break;
      }
      EXPECT_FALSE(swiftAccessCallEffects(Image, Low, Site, Binding));
    }
  }
}

TEST(NativeSourceHints,
     SwiftAccessScratchSurvivesOnlyDisjointWritesAndAllPaths) {
  for (unsigned Case = 0; Case < 4; ++Case) {
    SwiftAccessEffectFixture F;
    if (Case == 0) // architectural W-register zero extension
      F.word(4, 0x52800002);
    if (Case == 1) { // an untracked access need not have an end call
      F.word(7, 0xd503201f);
      F.word(8, 0xd503201f);
    }
    if (Case >= 2) {
      // Either outside the record or one byte at its last address.
      F.word(7, Case == 2 ? 0xf9001bff : 0x39009fff);
      F.word(8, 0x910043e0);
      F.word(9, 0x9400003b);
    }
    F.run();
    std::string Error;
    EXPECT_EQ(bool(F.infer(Error)), Case != 3) << Case << ": " << Error;
  }

  SwiftAccessEffectFixture F;
  ASSERT_TRUE(F.low());
  auto Low = *F.low();
  const auto Begin = swiftRuntimeSourceCallHint(F.Image, F.BeginSlot);
  const auto End = swiftRuntimeSourceCallHint(F.Image, F.EndSlot);
  ASSERT_TRUE(Begin);
  ASSERT_TRUE(End);
  NativeSourceCalls Calls;
  for (bool IsBegin : {true, false}) {
    auto &Contract = Calls[F.site(IsBegin)];
    Contract.Signature = IsBegin ? &Begin->Signature : &End->Signature;
    static_cast<SourceFrameEffects &>(Contract) = *F.effect(IsBegin);
  }
  ASSERT_TRUE(restoresNativeSourceState(Low, Arch::AArch64, Calls));
  // Split the real LowIR before and after beginAccess. A path that bypasses
  // initialization must not inherit the other predecessor's scratch token.
  auto Original = Low.Blocks.front();
  const auto BeginOp =
      std::find_if(Original.Ops.begin(), Original.Ops.end(),
                   [&](const LowOp &Op) { return Op.Addr == F.BeginSite; });
  const auto AfterBegin =
      std::find_if(BeginOp, Original.Ops.end(),
                   [&](const LowOp &Op) { return Op.Addr > F.BeginSite; });
  ASSERT_NE(BeginOp, Original.Ops.end());
  ASSERT_NE(AfterBegin, Original.Ops.end());
  Low.Blocks.resize(3);
  for (int I = 0; I < 3; ++I) {
    Low.Blocks[I] = {};
    Low.Blocks[I].Id = I;
  }
  Low.Blocks[0].StartAddr = Low.Entry;
  Low.Blocks[0].Ops.assign(Original.Ops.begin(), BeginOp);
  Low.Blocks[1].StartAddr = F.BeginSite;
  Low.Blocks[1].Ops.assign(BeginOp, AfterBegin);
  Low.Blocks[2].StartAddr = F.BeginSite + 4;
  Low.Blocks[2].Ops.assign(AfterBegin, Original.Ops.end());
  Low.Blocks[0].Succs = {1};
  Low.Blocks[1].Preds = {0};
  Low.Blocks[1].Succs = {2};
  Low.Blocks[2].Preds = {1};
  ASSERT_TRUE(restoresNativeSourceState(Low, Arch::AArch64, Calls));
  auto Expired = Low;
  auto &After = Expired.Blocks[2].Ops;
  const auto SP = NdVar::reg(a64reg::SP, 8);
  After.insert(After.begin(),
               {NativeVoidFrameFixture::op(NdOp::INT_ADD, SP,
                                           {SP, NdVar::cst(48, 8)}, 0x101c),
                NativeVoidFrameFixture::op(NdOp::INT_SUB, SP,
                                           {SP, NdVar::cst(48, 8)}, 0x101c)});
  EXPECT_FALSE(restoresNativeSourceState(Expired, Arch::AArch64, Calls));
  Low.Blocks[0].Succs.push_back(2);
  Low.Blocks[2].Preds.push_back(0);
  EXPECT_FALSE(restoresNativeSourceState(Low, Arch::AArch64, Calls));
  Low.Blocks[0].Succs = {1};
  Low.Blocks[1].Succs = {3};
  Low.Blocks[2].Preds = {3};
  LowBlock Loop;
  Loop.Id = 3;
  Loop.StartAddr = 0x1080;
  Loop.Preds = {1, 3};
  Loop.Succs = {3, 2};
  Loop.Ops = {NativeVoidFrameFixture::op(NdOp::INT_ADD,
                                         NdVar::reg(a64reg::X10, 8),
                                         {SP, NdVar::cst(16, 8)}, 0x1080),
              NativeVoidFrameFixture::op(
                  NdOp::COND_BR, {},
                  {NdVar::cst(0x1080, 8), NdVar::reg(a64reg::X9, 1)}, 0x1084)};
  Low.Blocks.push_back(Loop);
  ASSERT_TRUE(restoresNativeSourceState(Low, Arch::AArch64, Calls));
  // The zero-iteration path keeps the token, but a backedge that writes one
  // scratch byte must remove it from the fixed point before the final read.
  Low.Blocks[3].Ops.insert(
      Low.Blocks[3].Ops.end() - 1,
      NativeVoidFrameFixture::op(NdOp::STORE, {},
                                 {NdVar::reg(a64reg::X10, 8), NdVar::cst(0, 1)},
                                 0x1080));
  EXPECT_FALSE(restoresNativeSourceState(Low, Arch::AArch64, Calls));
}

TEST(NativeSourceHints, SwiftAccessEffectsRecheckScalarInferenceAndScratchABI) {
  SwiftAccessEffectFixture F(0, true);
  ASSERT_TRUE(F.med());
  std::string Error;
  ASSERT_TRUE(F.infer(Error)) << Error;
  const auto Begin = swiftRuntimeSourceCallHint(F.Image, F.BeginSlot);
  ASSERT_TRUE(Begin);
  for (unsigned Case = 0; Case < 14; ++Case) {
    auto Effects = *F.effect(true);
    auto &Scratch = *Effects.Scratch;
    switch (Case) {
    case 0:
      Scratch.Parameter = 2;
      break;
    case 1:
      Scratch.Bytes = 0;
      break;
    case 2:
      Scratch.Bytes = 8;
      break;
    case 3:
      Scratch.Condition->Parameter = 1;
      break;
    case 4:
      Scratch.Condition->Parameter = 4;
      break;
    case 5:
      Scratch.Condition->Values.clear();
      break;
    case 6:
      Scratch.TheAction = SourceFrameScratchEffect::Action::Finish;
      break;
    case 7:
      Effects.ReturnFrameOrExternal = SourceFrameReturnAlias{1, 24};
      break;
    case 8:
      Effects.ReadOnlyFrameParameters[1] = 24;
      break;
    case 9:
      for (unsigned I = 0; I < 17; ++I)
        Scratch.Condition->Values.insert(I);
      break;
    case 10:
      Scratch.Condition.reset();
      break;
    case 11:
      Scratch.RetainedValues.insert(34);
      break;
    case 12:
      Scratch.Condition->Values.erase(32);
      break;
    case 13:
      Scratch.TheDomain = static_cast<SourceFrameScratchEffect::Domain>(99);
      break;
    }
    EXPECT_FALSE(sourceFrameEffectsMatchABI(Effects, Begin->Signature)) << Case;
  }
  for (unsigned Case = 0; Case < 4; ++Case) {
    F.run();
    for (auto &Block : F.med()->Blocks)
      for (auto &Op : Block.Ops)
        if (Op.Addr == F.BeginSite && Op.SourceCallHint) {
          auto Binding =
              std::make_shared<SourceCallTypeHint>(*Op.SourceCallHint);
          if (Case == 0)
            Binding->Signature.Parameters[2].Type =
                NdType::makePtr(NdType::makeVoid());
          if (Case == 1)
            Binding->TargetName = "swift_release";
          if (Case == 2)
            Op.PreservesCallerSaved = true;
          if (Case == 3)
            Binding->WeakImport = true;
          Op.SourceCallHint = std::move(Binding);
        }
    EXPECT_FALSE(F.infer(Error)) << Case << ": " << Error;
  }
}

TEST(NativeSourceHints, ConsumedUIntRequiresCurrentTypeAndLifetimeEvidence) {
  using namespace swift_consumed_input_test;
  Fixture F;
  ASSERT_TRUE(F.Result.Success) << F.Result.Error;
  ASSERT_TRUE(F.low());
  auto Hints = buildSwiftConsumedInputCallHints(F.Image, *F.low());
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(Call);
  ASSERT_TRUE(Hint.SwiftConsumedInput);
  EXPECT_EQ(Hint.SwiftConsumedInput->MetadataSlot, MetadataSlot);
  EXPECT_EQ(Hint.SwiftConsumedInput->WitnessSlot, WitnessSlot);
  EXPECT_TRUE(Hint.requiresUniqueSourceOccurrence());
  const auto Effects = swiftConsumedInputCallEffects(F.Image, *F.low(), Hint);
  ASSERT_TRUE(Effects);
  EXPECT_EQ(Effects->WritableFrameParameters,
            (std::map<size_t, size_t>{{1, 8}}));
  EXPECT_EQ(Effects->InitializedFrameParameters, (std::set<size_t>{1}));
  EXPECT_TRUE(Effects->ReadOnlyFrameParameters.empty());
  EXPECT_FALSE(Effects->InitializesIndirectResult);
  std::string Error;
  const auto Entry = F.infer(Error);
  ASSERT_TRUE(Entry) << Error;
  ASSERT_EQ(Entry->Parameters.size(), 2U);
  EXPECT_EQ(Entry->Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(Entry->Parameters[1].Location.RegisterOffset, a64reg::X8);
  EXPECT_EQ(Entry->ReturnType->Kind, NdTypeKind::Void);
  F.EntrySignature = *Entry;
  F.runUInt();
  ASSERT_TRUE(F.Result.Success) << F.Result.Error;
  EXPECT_TRUE(validateSwiftConsumedInputBindings(F.Image, F.low(), *F.med()));
  auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image);
  EXPECT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  EXPECT_TRUE(
      sdk::SourceSwiftConsumedInputProjectionValidator(F.Image, F.Result)
          .valid(Bound.Function));
}

TEST(NativeSourceHints, ConsumedUIntRejectsStaleTypesInstructionsAndABI) {
  using namespace swift_consumed_input_test;
  Fixture F;
  ASSERT_TRUE(F.low());
  const auto Hints = buildSwiftConsumedInputCallHints(F.Image, *F.low());
  ASSERT_EQ(Hints.size(), 1U);
  for (unsigned Case = 0; Case < 34; ++Case) {
    SCOPED_TRACE(Case);
    auto Image = F.Image;
    auto Low = *F.low();
    auto Hint = Hints.at(Call);
    auto Change = [&](unsigned I, uint32_t W) {
      llvm::support::endian::write32le(Image.Segments[0].Data.data() + I * 4,
                                       W);
    };
    switch (Case) {
    case 0:
      Image.DyldBindSlots[MetadataSlot].Name = "_$sSiN";
      break;
    case 1:
      Image.DyldBindSlots[WitnessSlot].Name = "_$sSiSHsWP";
      break;
    case 2:
      Image.DyldBindSlots[RuntimeSlot].Name += "wrong";
      break;
    case 3:
      Image.DyldBindSlots[MetadataSlot].WeakImport = true;
      break;
    case 4:
      Image.DyldBindSlots[WitnessSlot].WeakImport = true;
      break;
    case 5:
      Image.DyldBindSlots[RuntimeSlot].WeakImport = true;
      break;
    case 6:
      Image.DyldBindSlots[MetadataSlot].Addend = 8;
      break;
    case 7:
      Image.DyldBindSlots[WitnessSlot].Addend = 8;
      break;
    case 8:
      Image.DyldBindSlots[RuntimeSlot].Addend = 8;
      break;
    case 9:
      Image.DyldBindSlots[MetadataSlot].Module = "other";
      break;
    case 10:
      Image.DyldBindSlots[WitnessSlot].Module = "other";
      break;
    case 11:
      Image.DyldBindSlots[RuntimeSlot].Module = "other";
      break;
    case 12:
      Image.ImportPtrSlots[MetadataSlot] = "_$sSiN";
      break;
    case 13:
      Change(5, 0xb9400421);
      break;
    case 14:
      Change(7, 0xb9400842);
      break;
    case 15:
      Change(8, 0xd2800001);
      break;
    case 16:
      Low.Blocks[0].InstructionBoundaries.clear();
      break;
    case 17:
      Low.Blocks[0].Succs.push_back(123);
      break;
    case 18:
      for (auto &B : Low.Blocks)
        for (auto &O : B.Ops)
          if (O.Addr == Call - 16 && O.Output.Size)
            O.Output.Size = 4;
      break;
    case 19:
      Hint.Signature.Parameters[1].Location.ValueBytes = 4;
      break;
    case 20:
      Hint.Signature.Parameters[0].Location.RegisterOffset = a64reg::X9;
      break;
    case 21:
      Hint.Signature.Parameters[2].TheRole =
          SourceParameterTypeHint::Role::SwiftContext;
      break;
    case 22:
      Hint.Signature.Parameters.pop_back();
      break;
    case 23:
      Hint.SwiftConsumedInput->MetadataSlot += 8;
      break;
    case 24:
      Hint.SwiftConsumedInput->WitnessSlot -= 8;
      break;
    case 25:
      Hint.SwiftConsumedInput->Site.Instruction += 4;
      break;
    case 26:
      Hint.SwiftConsumedInput->FunctionEntry += 4;
      break;
    case 27:
      Hint.SwiftConsumedInput.reset();
      break;
    case 28:
      Hint.WeakImport = true;
      break;
    case 29:
      Hint.DoesNotReturn = true;
      break;
    case 30:
      Image.Segments[1].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      break;
    case 31:
      Image.Arch = Arch::X64;
      break;
    case 32:
      Image.Format = BinaryFormat::ELF;
      break;
    case 33:
      Image.IsRelocatable = true;
      break;
    }
    EXPECT_FALSE(swiftConsumedInputCallEffects(Image, Low, Hint));
  }
}

TEST(NativeSourceHints,
     ConsumedUIntCannotReadUninitializedOrConsumedFrameBytes) {
  using namespace swift_consumed_input_test;
  for (unsigned Case = 0; Case < 6; ++Case) {
    SCOPED_TRACE(Case);
    Fixture F;
    switch (Case) {
    case 0:
      F.word(3, 0xb9000be0);
      break; // four-byte initialization
    case 1:
      F.word(3, 0xd503201f);
      break; // missing initialization
    case 2:
      F.word(8, 0x910043e0);
      break; // overlaps saved frame/link words
    case 3:
      F.word(10, 0xf94007e9);
      F.word(11, 0xa9417bfd);
      F.word(12, 0x910083ff);
      F.word(13, 0xd65f03c0);
      F.Image.Symbols[0].Size = 56;
      break;
    case 4:
      F.word(3, 0xf90007ff);
      F.word(8, 0x910073e0);
      break; // range extends past active frame
    case 5:
      F.word(3, 0xf90007ff);
      F.word(8, 0x910003e0);
      break; // unwritten neighboring word
    }
    F.runUInt();
    ASSERT_TRUE(F.Result.Success) << F.Result.Error;
    std::string Error;
    EXPECT_FALSE(F.infer(Error)) << Error;
  }
}

TEST(NativeSourceHints,
     ConsumedUIntPublicationReplaysEveryCurrentRepresentation) {
  using namespace swift_consumed_input_test;
  for (unsigned Case = 0; Case < 18; ++Case) {
    SCOPED_TRACE(Case);
    Fixture F;
    std::string Error;
    auto EntryHint = F.infer(Error);
    ASSERT_TRUE(EntryHint) << Error;
    F.EntrySignature = *EntryHint;
    F.runUInt();
    ASSERT_TRUE(F.Result.Success) << F.Result.Error;
    auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    auto &Published = Bound.Function;
    HighExpr *CallExpr = nullptr;
    walkStmts(Published.Body, [&](const HighStmt &S) {
      forEachExpr(S, [&](const ExprPtr &E) {
        if (E && E->SourceCallHint && E->SourceCallHint->SwiftConsumedInput)
          CallExpr = E.get();
      });
    });
    ASSERT_TRUE(CallExpr);
    MedOp *CallOp = nullptr;
    for (auto &B : F.med()->Blocks)
      for (auto &O : B.Ops)
        if (O.SourceCallHint && O.SourceCallHint->SwiftConsumedInput)
          CallOp = &O;
    ASSERT_TRUE(CallOp);
    auto ClearMed = [&] {
      auto H = std::make_shared<SourceCallTypeHint>(*CallOp->SourceCallHint);
      H->SwiftConsumedInput.reset();
      CallOp->SourceCallHint = H;
    };
    auto ClearHigh = [&] {
      auto H = std::make_shared<SourceCallTypeHint>(*CallExpr->SourceCallHint);
      H->SwiftConsumedInput.reset();
      CallExpr->SourceCallHint = H;
    };
    switch (Case) {
    case 0:
      CallExpr->Operands[0] = HighExpr::makeConst(0, 8);
      break;
    case 1:
      CallExpr->Operands[1] = HighExpr::makeConst(0, 8);
      break;
    case 2:
      CallExpr->Operands[2] = CallExpr->Operands[3];
      break;
    case 3:
      CallExpr->Operands[3] = CallExpr->Operands[2];
      break;
    case 4:
      ClearMed();
      break;
    case 5:
      ClearHigh();
      break;
    case 6:
      ClearMed();
      ClearHigh();
      break;
    case 7:
      CallOp->Inputs[1] = CallOp->Inputs[2];
      break;
    case 8:
      CallOp->Inputs[2] = CallOp->Inputs[3];
      break;
    case 9:
      CallOp->Inputs[3] = CallOp->Inputs[4];
      break;
    case 10:
      for (size_t I = 0; I < Published.Body.size(); ++I)
        if (Published.Body[I].Kind == StmtKind::Store) {
          Published.Body.erase(Published.Body.begin() + I);
          break;
        }
      break;
    case 11:
      for (size_t I = 0; I < Published.Body.size(); ++I)
        if (Published.Body[I].Kind == StmtKind::Call) {
          Published.Body.insert(Published.Body.begin() + I, Published.Body[I]);
          break;
        }
      break;
    case 12:
      Published.FrameSize += 16;
      break;
    case 13:
      CallOp->OriginSeq += 1;
      break;
    case 14:
      F.Image.DyldBindSlots[MetadataSlot].Name = "_$sSiN";
      break;
    case 15: {
      auto H = std::make_shared<SourceCallTypeHint>(*CallExpr->SourceCallHint);
      H->Signature.Parameters.pop_back();
      CallExpr->SourceCallHint = H;
    } break;
    case 16:
      F.word(3, 0xb9000be0);
      break;
    case 17:
      F.Result.FunctionAudits.front().MedIRVerified = false;
      break;
    }
    EXPECT_FALSE(
        sdk::SourceSwiftConsumedInputProjectionValidator(F.Image, F.Result)
            .valid(Published));
  }
}

TEST(NativeSourceHints, GenericAnyHashableDoesNotClaimConsumedUIntEffects) {
  using namespace swift_consumed_input_test;
  Fixture F;
  for (const auto &[Slot, Name] : std::map<va_t, std::string>{
           {MetadataSlot, "_$sSiN"}, {WitnessSlot, "_$sSiSHsWP"}}) {
    F.Image.ImportPtrSlots[Slot] = Name;
    F.Image.DyldBindSlots[Slot].Name = Name;
  }
  // A scalar result permits an internal typed body independently of the
  // private input's lifetime. It cannot turn a generic call into a UInt
  // consumption proof or authorize publishing its private-frame use.
  F.word(10, 0xd28000e0); // mov x0, #7
  F.word(11, 0xa9417bfd);
  F.word(12, 0x910083ff);
  F.word(13, 0xd65f03c0);
  F.Image.Symbols[0].Size = 56;
  F.runUInt();
  ASSERT_TRUE(F.Result.Success) << F.Result.Error;
  ASSERT_TRUE(F.low());
  EXPECT_TRUE(buildSwiftConsumedInputCallHints(F.Image, *F.low()).empty());
  EXPECT_TRUE(validateSwiftConsumedInputBindings(F.Image, F.low(), *F.med()));
  NativeSourceCalls Calls;
  for (const auto &Block : F.med()->Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::CALL) {
        ASSERT_TRUE(Op.SourceCallHint);
        EXPECT_FALSE(Op.SourceCallHint->SwiftConsumedInput);
        EXPECT_FALSE(swiftConsumedInputCallEffects(F.Image, *F.low(),
                                                   *Op.SourceCallHint));
        Calls[{Op.Addr, Op.OriginSeq}].Signature =
            &Op.SourceCallHint->Signature;
      }
  ASSERT_EQ(Calls.size(), 1U);
  EXPECT_FALSE(restoresNativeSourceState(*F.low(), Arch::AArch64, Calls));
  std::string Error;
  const auto Hint = F.infer(Error);
  ASSERT_TRUE(Hint) << Error;
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Int);
}

TEST(NativeSourceHints, ScalarResultCannotSkipConsumedUIntStateProof) {
  using namespace swift_consumed_input_test;
  for (unsigned Case = 0; Case < 6; ++Case) {
    SCOPED_TRACE(Case);
    Fixture F;
    F.word(10, 0xd28000e0); // mov x0, #7
    F.word(11, 0xa9417bfd);
    F.word(12, 0x910083ff);
    F.word(13, 0xd65f03c0);
    F.Image.Symbols[0].Size = 56;
    if (Case == 1)
      F.word(3, 0xb9000be0); // Only half the input is initialized.
    if (Case == 2)
      F.word(3, 0xd503201f); // Missing initialization.
    if (Case == 3)
      F.word(10, 0xf94007e0); // Read the consumed input as the result.
    F.runUInt();
    ASSERT_TRUE(F.Result.Success) << F.Result.Error;
    if (Case >= 4) {
      for (auto &Block : F.med()->Blocks)
        for (auto &Op : Block.Ops)
          if (Op.SourceCallHint && Op.SourceCallHint->SwiftConsumedInput) {
            auto Hint =
                std::make_shared<SourceCallTypeHint>(*Op.SourceCallHint);
            if (Case == 4)
              Hint->SwiftConsumedInput.reset();
            else
              Hint->Signature.Parameters[1].Location.ValueBytes = 4;
            Op.SourceCallHint = std::move(Hint);
          }
    }
    std::string Error;
    const auto Hint = F.infer(Error);
    EXPECT_EQ(bool(Hint), Case == 0) << Error;
  }
}

TEST(NativeSourceHints, ConsumedObjectIdentifierRequiresItsOwnTypeContract) {
  using namespace swift_consumed_input_test;
  IdentifierFixture F;
  ASSERT_TRUE(F.Result.Success) << F.Result.Error;
  ASSERT_TRUE(F.low());
  const auto Hints = buildSwiftConsumedInputCallHints(F.Image, *F.low());
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(IdentifierCall);
  const auto Effect = swiftConsumedInputCallEffects(F.Image, *F.low(), Hint);
  ASSERT_TRUE(Effect);
  EXPECT_EQ(Effect->WritableFrameParameters,
            (std::map<size_t, size_t>{{1, 8}}));
  EXPECT_EQ(Effect->InitializedFrameParameters, (std::set<size_t>{1}));
  EXPECT_TRUE(Effect->ReadOnlyFrameParameters.empty());
  EXPECT_FALSE(Effect->InitializesIndirectResult);
  std::string Error;
  const auto EntryHint = F.infer(Error);
  ASSERT_TRUE(EntryHint) << Error;
  ASSERT_EQ(EntryHint->Parameters.size(), 2U);
  EXPECT_EQ(EntryHint->Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(EntryHint->Parameters[1].Location.RegisterOffset, a64reg::X8);
  F.EntrySignature = *EntryHint;
  F.runUInt();
  const auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  EXPECT_TRUE(
      sdk::SourceSwiftConsumedInputProjectionValidator(F.Image, F.Result)
          .valid(Bound.Function));
}

TEST(NativeSourceHints, ConsumedIdentifierRechecksTypeWindowAndCarrier) {
  using namespace swift_consumed_input_test;
  IdentifierFixture F;
  ASSERT_TRUE(F.low());
  const auto Hints = buildSwiftConsumedInputCallHints(F.Image, *F.low());
  ASSERT_EQ(Hints.size(), 1U);
  for (unsigned Case = 0; Case < 25; ++Case) {
    SCOPED_TRACE(Case);
    auto Image = F.Image;
    auto Low = *F.low();
    auto Hint = Hints.at(IdentifierCall);
    auto Change = [&](unsigned I, uint32_t Word) {
      llvm::support::endian::write32le(Image.Segments[0].Data.data() + I * 4,
                                       Word);
    };
    switch (Case) {
    case 0:
      Image.DyldBindSlots[MetadataSlot].Name = "_$sSuN";
      break;
    case 1:
      Image.DyldBindSlots[WitnessSlot].Name = "_$sSuSHsWP";
      break;
    case 2:
      Image.DyldBindSlots[MetadataSlot].WeakImport = true;
      break;
    case 3:
      Image.DyldBindSlots[WitnessSlot].Module = "other";
      break;
    case 4:
      Image.DyldBindSlots[WitnessSlot].Addend = 8;
      break;
    case 5:
      Image.ImportPtrSlots[MetadataSlot] = "_$sSuN";
      break;
    case 6:
      Change(8, 0x1100a108);
      break; // partial result computation
    case 7:
      Change(8, 0x9100a101);
      break; // destroys metadata identity
    case 8:
      Change(8, 0x9100a102);
      break; // destroys witness identity
    case 9:
      Change(8, 0x94000038);
      break; // intervening call
    case 10:
      Change(8, 0xf9400108);
      break; // memory observation, not ADD
    case 11:
      Change(8, 0xd503201f);
      break; // unsupported extra instruction
    case 12:
      Change(9, 0xd2800001);
      break; // final instruction clobber
    case 13:
      Change(7, 0xb9400842);
      break; // narrow witness
    case 14:
      Low.Blocks.front().InstructionBoundaries.clear();
      break;
    case 15:
      Low.Blocks.front().Succs.push_back(0x2000);
      break;
    case 16:
      for (auto &Block : Low.Blocks)
        for (auto &Op : Block.Ops)
          if (Op.Addr == IdentifierCall - 8 && Op.Output.Size)
            Op.Output.Size = 4;
      break;
    case 17:
      Hint.Signature.Parameters[1].Location.ValueBytes = 4;
      break;
    case 18:
      Hint.Signature.Parameters[0].Location.RegisterOffset = a64reg::X9;
      break;
    case 19:
      Hint.SwiftConsumedInput->MetadataSlot = WitnessSlot;
      break;
    case 20:
      Hint.SwiftConsumedInput->Site.Instruction -= 4;
      break;
    case 21:
      Hint.SwiftConsumedInput.reset();
      break;
    case 22:
      for (auto &Section : Image.Sections)
        if (Section.VA == RuntimeSlot)
          Section.Type = llvm::MachO::S_THREAD_LOCAL_VARIABLE_POINTERS;
      break;
    }
    if (Case == 23 || Case == 24) {
      // Only the runtime slot is TLS/lazy: metadata and witness remain
      // ordinary GOT identities and cannot mask a bad callee import.
      auto *Got = const_cast<Section *>(Image.getSectionFor(RuntimeSlot));
      ASSERT_TRUE(Got);
      Section RuntimeSection = *Got;
      Got->VA = MetadataSlot;
      Got->Size = Got->FileSz = 16;
      RuntimeSection.Size = RuntimeSection.FileSz = 8;
      RuntimeSection.Type = Case == 23
                                ? llvm::MachO::S_THREAD_LOCAL_VARIABLE_POINTERS
                                : llvm::MachO::S_LAZY_SYMBOL_POINTERS;
      Image.Sections.push_back(RuntimeSection);
    }
    EXPECT_FALSE(swiftConsumedInputCallEffects(Image, Low, Hint));
  }
  // The result-address window does not choose a type by its extent. A
  // complete current UInt pair is independently valid; a mixed pair is not.
  for (const auto &[Slot, Name] : std::map<va_t, std::string>{
           {MetadataSlot, "_$sSuN"}, {WitnessSlot, "_$sSuSHsWP"}}) {
    F.Image.ImportPtrSlots[Slot] = Name;
    F.Image.DyldBindSlots[Slot].Name = Name;
  }
  F.runUInt();
  const auto UInt = buildSwiftConsumedInputCallHints(F.Image, *F.low());
  ASSERT_EQ(UInt.size(), 1U);
  EXPECT_TRUE(swiftConsumedInputCallEffects(F.Image, *F.low(),
                                            UInt.at(IdentifierCall)));
}

TEST(NativeSourceHints, ConsumedIdentifierCannotSkipInputLifetime) {
  using namespace swift_consumed_input_test;
  for (unsigned Case = 0; Case < 9; ++Case) {
    SCOPED_TRACE(Case);
    IdentifierFixture F;
    F.word(11,
           0xd28000e0); // Defined scalar result must not bypass state proof.
    F.word(12, 0xa9417bfd);
    F.word(13, 0x910083ff);
    F.word(14, 0xd65f03c0);
    F.Image.Symbols[0].Size = 60;
    if (Case == 1)
      F.word(3, 0xb9000be0); // partial initial value
    if (Case == 2)
      F.word(3, 0xd503201f); // no initial value
    if (Case == 3)
      F.word(11, 0xf94007e0); // consumed bytes as the result
    if (Case == 4)
      F.word(9, 0x910003e0); // unwritten adjacent word
    if (Case == 5)
      F.word(9, 0x910043e0); // saved frame word
    if (Case == 6)
      F.word(9, 0x910073e0); // partial active-frame range
    F.runUInt();
    ASSERT_TRUE(F.Result.Success) << F.Result.Error;
    if (Case >= 7) {
      for (auto &Block : F.med()->Blocks)
        for (auto &Op : Block.Ops)
          if (Op.SourceCallHint && Op.SourceCallHint->SwiftConsumedInput) {
            auto Hint =
                std::make_shared<SourceCallTypeHint>(*Op.SourceCallHint);
            if (Case == 7)
              Hint->SwiftConsumedInput.reset();
            else
              Hint->Signature.Parameters[1].Location.ValueBytes = 4;
            Op.SourceCallHint = std::move(Hint);
          }
    }
    std::string Error;
    EXPECT_EQ(bool(F.infer(Error)), Case == 0) << Error;
  }
}

TEST(NativeSourceHints, ConsumedIdentifierPublicationReplaysCurrentOperands) {
  using namespace swift_consumed_input_test;
  for (unsigned Case = 0; Case < 12; ++Case) {
    SCOPED_TRACE(Case);
    IdentifierFixture F;
    std::string Error;
    const auto EntryHint = F.infer(Error);
    ASSERT_TRUE(EntryHint) << Error;
    F.EntrySignature = *EntryHint;
    F.runUInt();
    auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    HighExpr *CallExpr = nullptr;
    walkStmts(Bound.Function.Body, [&](const HighStmt &S) {
      forEachExpr(S, [&](const ExprPtr &E) {
        if (E && E->SourceCallHint && E->SourceCallHint->SwiftConsumedInput)
          CallExpr = E.get();
      });
    });
    ASSERT_TRUE(CallExpr);
    MedOp *CallOp = nullptr;
    for (auto &Block : F.med()->Blocks)
      for (auto &Op : Block.Ops)
        if (Op.SourceCallHint && Op.SourceCallHint->SwiftConsumedInput)
          CallOp = &Op;
    ASSERT_TRUE(CallOp);
    if (Case < 4)
      CallExpr->Operands[Case] = HighExpr::makeConst(0, 8);
    if (Case == 4 || Case == 6) {
      auto Hint = std::make_shared<SourceCallTypeHint>(*CallOp->SourceCallHint);
      Hint->SwiftConsumedInput.reset();
      CallOp->SourceCallHint = Hint;
    }
    if (Case == 5 || Case == 6) {
      auto Hint =
          std::make_shared<SourceCallTypeHint>(*CallExpr->SourceCallHint);
      Hint->SwiftConsumedInput.reset();
      CallExpr->SourceCallHint = Hint;
    }
    if (Case == 7)
      CallOp->Inputs[2] = CallOp->Inputs[3];
    if (Case == 8)
      F.word(3, 0xb9000be0);
    if (Case == 9)
      F.Result.FunctionAudits.front().MedIRVerified = false;
    if (Case == 10)
      for (size_t I = 0; I < Bound.Function.Body.size(); ++I)
        if (Bound.Function.Body[I].Kind == StmtKind::Call) {
          Bound.Function.Body.insert(Bound.Function.Body.begin() + I,
                                     Bound.Function.Body[I]);
          break;
        }
    if (Case == 11)
      for (size_t I = 0; I < Bound.Function.Body.size(); ++I)
        if (Bound.Function.Body[I].Kind == StmtKind::Store) {
          Bound.Function.Body.erase(Bound.Function.Body.begin() + I);
          break;
        }
    EXPECT_FALSE(
        sdk::SourceSwiftConsumedInputProjectionValidator(F.Image, F.Result)
            .valid(Bound.Function));
  }
}

TEST(NativeSourceHints,
     HFAFieldTypeRequiresCurrentCallAndFrameProofForPublication) {
  constexpr auto Provider =
      "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics";
  HFAImportVeneerFixture F("_CGRectStandardize", Provider);
  constexpr va_t Caller = 0x1040;
  const uint32_t Words[] = {0xa9bf7bfd, 0x910003fd, 0x97ffffee, 0xa8c17bfd,
                            0xd65f03c0};
  for (unsigned I = 0; I != 5; ++I)
    llvm::support::endian::write32le(
        F.Image.Segments[0].Data.data() + 0x40 + 4 * I, Words[I]);
  F.Image.Symbols.push_back({"hfa_caller", Caller, 20, true});
  F.Options.OnlyFunctionEntries.insert(Caller);
  F.Result = Pipeline().run(F.Image, F.Context, F.Options);
  ASSERT_TRUE(F.Result.Success) << F.Result.Error;
  std::map<va_t, std::string> Diagnostics;
  ASSERT_GT(sdk::inferObjCNativeDependencies(F.Image, F.Result, F.Options,
                                             Diagnostics, {Caller}),
            0U);
  ASSERT_TRUE(F.Options.SourceTypeHints.count(Caller)) << Diagnostics[Caller];
  const auto Scalar = F.Options.SourceTypeHints.at(Caller);
  ASSERT_TRUE(Scalar.ReturnType);
  EXPECT_EQ(Scalar.ReturnType->Kind, NdTypeKind::Float);
  EXPECT_EQ(Scalar.ReturnType->Size, 8U);
  auto Relifted = Pipeline().run(F.Image, F.Context, F.Options);
  ASSERT_TRUE(Relifted.Success) << Relifted.Error;
  const auto H =
      std::find_if(Relifted.HighFuncs.begin(), Relifted.HighFuncs.end(),
                   [](const auto &Fn) { return Fn.Entry == Caller; });
  const auto A = std::find_if(
      Relifted.FunctionAudits.begin(), Relifted.FunctionAudits.end(),
      [](const auto &Audit) { return Audit.Entry == Caller; });
  ASSERT_NE(H, Relifted.HighFuncs.end());
  ASSERT_NE(A, Relifted.FunctionAudits.end());
  const auto Bound = sdk::bindObjCSourceReferences(*H, F.Image);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  std::map<va_t, const HighFunc *> Functions;
  for (const auto &Fn : Relifted.HighFuncs)
    Functions.emplace(Fn.Entry, &Fn);
  const auto Allowed = [&](const HighExpr &Call) {
    return sdk::objcSourceCallBound(Call, F.Image, Functions);
  };
  EXPECT_TRUE(
      sdk::sourceBodyLimitation(Bound.Function, Scalar, &*A, Allowed).empty());
  // A field's type cannot authenticate another provider or a missing saved LR.
  for (unsigned Mutation = 0; Mutation != 2; ++Mutation) {
    auto Image = F.Image;
    if (Mutation == 0)
      Image.DyldBindSlots.at(F.Slot).Module = "/not/CoreGraphics";
    else
      llvm::support::endian::write32le(Image.Segments[0].Data.data() + 0x4c,
                                       0xd503201f); // No saved LR/SP restore.
    PipelineOptions Unbound;
    Unbound.OnlyFunctionEntries = F.Options.OnlyFunctionEntries;
    Unbound.EmitDumpOutput = false;
    auto Result = Pipeline().run(Image, F.Context, Unbound);
    ASSERT_TRUE(Result.Success);
    Diagnostics.clear();
    sdk::inferObjCNativeDependencies(Image, Result, Unbound, Diagnostics,
                                     {Caller});
    EXPECT_FALSE(Unbound.SourceTypeHints.count(Caller)) << Mutation;
  }
}

TEST(NativeSourceHints, RecoveredCallPointerUsesKeepWidthsAndEntryIdentity) {
  for (auto Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
      SCOPED_TRACE(unsigned(Architecture));
      SCOPED_TRACE(Mutation);
      const auto &Registers = getTargetRegInfo(Architecture);
      const auto Bytes = Registers.PointerSize;
      MedFunc Function;
      Function.Entry = 0x1000;
      Function.Blocks.resize(1);
      auto &Block = Function.Blocks.front();
      Block.Id = 0;
      std::vector<MedVar> Inputs;
      for (unsigned I = 0; I != 2; ++I) {
        MedVar Parameter;
        Parameter.Kind = MedVar::Param;
        Parameter.Id = I;
        Parameter.Size = Bytes;
        Parameter.RegOff = Registers.IntParamRegs[I];
        Parameter.TheArch = Architecture;
        Function.Params.push_back(Parameter);
        auto Input = Parameter;
        Input.Kind = MedVar::Reg;
        Input.Id += 100;
        Inputs.push_back(Input);
      }
      MedOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(MedVar::makeConst(0x3000, Bytes));
      Block.Ops.push_back(Call);
      MedCallInfo Info;
      Info.BlockId = 0;
      Info.OpIdx = 0;
      Info.TargetAddr = 0x3000;
      Info.Args = Inputs;
      auto Pointer = NdType::makePtr(NdType::makeVoid());
      Pointer->Size = Bytes;
      std::map<va_t, std::vector<TypeRef>> Parameters{
          {0x3000, {Pointer, Pointer}}};
      if (Mutation == 1) {
        Parameters[0x3000].pop_back();
      } else if (Mutation == 2) {
        MedOp Copy;
        Copy.Opcode = NdOp::COPY;
        Copy.Output.Kind = MedVar::Temp;
        Copy.Output.Id = 200;
        Copy.Output.Size = Bytes / 2;
        Copy.addInput(Inputs[0]);
        Block.Ops.push_back(Copy);
      } else if (Mutation == 3) {
        ++Info.Args[0].SSAVer;
      } else if (Mutation == 4) {
        MedCallClobber Clobber;
        Clobber.Value = Inputs[0];
        Function.CallClobbers.push_back(Clobber);
      } else if (Mutation == 5) {
        auto Opaque = Info;
        Opaque.TargetAddr = 0x4000;
        Opaque.OpIdx = 1;
        Opaque.Args.resize(1);
        Function.CallInfos.push_back(Opaque);
        auto Operation = Call;
        Operation.Inputs[0] = MedVar::makeConst(0x4000, Bytes);
        Block.Ops.push_back(Operation);
      } else if (Mutation == 6) {
        Block.Ops[0].addInput(MedVar::makeConst(7, Bytes));
        Block.Ops[0].addInput(Inputs[1]);
      } else if (Mutation == 7) {
        // Reverse block order and carry the full pointer through a loop PHI.
        MedBlock Producer;
        Producer.Id = 1;
        MedOp Copy;
        Copy.Opcode = NdOp::COPY;
        Copy.Output.Kind = MedVar::Temp;
        Copy.Output.Id = 200;
        Copy.Output.Size = Bytes;
        Copy.addInput(Inputs[0]);
        Producer.Ops.push_back(Copy);
        PhiNode Phi;
        Phi.Output = Copy.Output;
        ++Phi.Output.Id;
        Phi.Args = {{1, Copy.Output}, {0, Phi.Output}};
        Block.Phis.push_back(Phi);
        Info.Args[0] = Phi.Output;
        Function.Blocks.push_back(std::move(Producer));
      }
      if (Mutation == 8) {
        // A later W0/EAX write has its own SSA identity. It does not redefine
        // the full entry pointer merely because the physical offsets coincide.
        MedOp Narrow;
        Narrow.Opcode = NdOp::COPY;
        Narrow.Output = Inputs[0];
        Narrow.Output.Id += 300;
        Narrow.Output.Size = Bytes / 2;
        Narrow.addInput(MedVar::makeConst(7, Bytes / 2));
        Block.Ops.push_back(Narrow);
      }
      if (Mutation == 9) {
        // The same admitted call can retain all arguments as explicit operands.
        Block.Ops[0].addInput(Inputs[0]);
        Block.Ops[0].addInput(Inputs[1]);
      } else {
        Function.CallInfos.push_back(Info);
      }
      const auto Pointers =
          inferMedSourcePointerParameters(Function, Architecture, Parameters);
      ASSERT_EQ(Pointers.size(), 2U);
      EXPECT_EQ(Pointers[0], Mutation == 0 || Mutation >= 7);
      EXPECT_EQ(Pointers[1], Mutation != 1 && Mutation != 6);
    }
}
