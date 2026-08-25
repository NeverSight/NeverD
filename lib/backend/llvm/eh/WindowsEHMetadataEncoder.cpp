//===- WindowsEHMetadataEncoder.cpp - Windows EH metadata encoder ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Canonical serialization of normalized Windows exception information into
/// LLVM metadata.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/WindowsEHMetadataEncoder.h"

#include "MedLLVMEHHelpers.h"

#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Metadata.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::windows_eh_md {

using med_llvm_eh::hexBytes;
using med_llvm_eh::mdSInt;
using med_llvm_eh::mdUInt;

llvm::MDNode *getCanonicalFunctionMetadata(llvm::LLVMContext &Context,
                                           const ExceptionFunction &EH,
                                           Arch TargetArch,
                                           BinaryFormat TargetFormat) {
  auto Str = [&](llvm::StringRef Value) -> llvm::Metadata * {
    return llvm::MDString::get(Context, Value);
  };
  auto Node = [&](const std::vector<llvm::Metadata *> &Values) {
    return llvm::MDNode::get(Context, Values);
  };

  auto UnwindOp = [&](const UnwindOperation &Op) -> llvm::Metadata * {
    return Node(
        {mdUInt(Context, static_cast<uint8_t>(Op.Kind), 8),
         mdUInt(Context, Op.CodeOffset, 32), mdUInt(Context, Op.OpInfo, 8),
         mdUInt(Context, Op.SlotCount, 8), mdUInt(Context, Op.Register, 16),
         mdUInt(Context, Op.StackOffset), Str(hexBytes(Op.OperandBytes)),
         mdUInt(Context, static_cast<uint8_t>(Op.RegisterClass), 8),
         mdUInt(Context, Op.RegisterMask, 32),
         mdUInt(Context, Op.InstructionSize, 8)});
  };

  std::vector<llvm::Metadata *> UnwindOps;
  UnwindOps.reserve(EH.UnwindOperations.size());
  for (const UnwindOperation &Op : EH.UnwindOperations)
    UnwindOps.push_back(UnwindOp(Op));

  std::vector<llvm::Metadata *> Epilogs;
  Epilogs.reserve(EH.Epilogs.size());
  for (const UnwindEpilog &Epilog : EH.Epilogs) {
    std::vector<llvm::Metadata *> Ops;
    Ops.reserve(Epilog.Operations.size());
    for (const UnwindOperation &Op : Epilog.Operations)
      Ops.push_back(UnwindOp(Op));
    Epilogs.push_back(Node(
        {mdSInt(Context, Epilog.StartOffset), mdUInt(Context, Epilog.Flags, 8),
         mdUInt(Context, Epilog.FirstOperationOffset, 32),
         mdUInt(Context, Epilog.LastInstructionOffset, 32), Node(Ops)}));
  }

  std::vector<llvm::Metadata *> SEHScopes;
  if (EH.SEH) {
    SEHScopes.reserve(EH.SEH->Scopes.size());
    for (const SEHScopeRecord &Scope : EH.SEH->Scopes)
      SEHScopes.push_back(
          Node({mdUInt(Context, Scope.GuardedRange.Begin),
                mdUInt(Context, Scope.GuardedRange.End),
                mdUInt(Context, static_cast<uint8_t>(Scope.Kind), 8),
                mdUInt(Context, Scope.FilterOrFinallyVA),
                mdUInt(Context, Scope.NormalizedFilterVA),
                mdUInt(Context, Scope.HandlerVA),
                mdUInt(Context, Scope.ContinuationVA),
                Str(getExceptionParseStatusName(Scope.ParseStatus))}));
  }

  std::vector<llvm::Metadata *> CxxUnwind;
  std::vector<llvm::Metadata *> CxxTry;
  std::vector<llvm::Metadata *> CxxIP;
  llvm::Metadata *CxxHeader = nullptr;
  if (EH.Cxx) {
    const CxxExceptionInfo &Cxx = *EH.Cxx;
    std::vector<llvm::Metadata *> CxxSpecTypes;
    for (const CxxExceptionSpecType &Spec : Cxx.ExceptionSpecTypes)
      CxxSpecTypes.push_back(Node({mdUInt(Context, Spec.Adjectives, 32),
                                   mdUInt(Context, Spec.TypeDescriptorVA)}));
    CxxHeader = Node(
        {mdUInt(Context, static_cast<uint8_t>(Cxx.NativeEncoding), 8),
         mdUInt(Context, Cxx.Magic, 32), mdUInt(Context, Cxx.Flags, 32),
         mdUInt(Context, Cxx.MaxState, 32),
         mdSInt(Context, Cxx.UnwindHelpOffset, 32),
         mdUInt(Context, Cxx.ESTypeListVA), mdUInt(Context, Cxx.BBTFlags, 32),
         mdUInt(Context, Cxx.FrameOffset, 32),
         mdUInt(Context, Cxx.IsCatchFunclet, 1),
         mdUInt(Context, Cxx.IsSeparated, 1),
         mdUInt(Context, Cxx.IsSynchronous, 1),
         mdUInt(Context, Cxx.IsNoExcept, 1),
         mdUInt(Context, static_cast<uint8_t>(Cxx.Version), 8),
         mdUInt(Context, Cxx.HasDynamicStackAlignment, 1), Node(CxxSpecTypes),
         mdUInt(Context, Cxx.NativeFuncInfoVA)});
    for (const CxxUnwindAction &Action : Cxx.UnwindMap)
      CxxUnwind.push_back(
          Node({mdSInt(Context, Action.ToState, 32),
                mdUInt(Context, Action.ActionVA),
                mdUInt(Context, static_cast<uint8_t>(Action.Kind), 8),
                mdSInt(Context, Action.ObjectOffset, 32)}));
    for (const CxxTryBlock &Try : Cxx.TryBlocks) {
      std::vector<llvm::Metadata *> Catches;
      for (const CxxCatchHandler &Catch : Try.Handlers) {
        std::vector<llvm::Metadata *> Continuations;
        for (va_t Address : Catch.ContinuationVAs)
          Continuations.push_back(mdUInt(Context, Address));
        Catches.push_back(Node({mdUInt(Context, Catch.Adjectives, 32),
                                mdUInt(Context, Catch.TypeDescriptorVA),
                                mdSInt(Context, Catch.CatchObjectOffset, 32),
                                mdUInt(Context, Catch.HandlerVA),
                                mdSInt(Context, Catch.ParentFrameOffset, 32),
                                Node(Continuations)}));
      }
      CxxTry.push_back(Node(
          {mdSInt(Context, Try.TryLow, 32), mdSInt(Context, Try.TryHigh, 32),
           mdSInt(Context, Try.CatchHigh, 32), Node(Catches)}));
    }
    for (const CxxIPState &IP : Cxx.IPMap)
      CxxIP.push_back(
          Node({mdUInt(Context, IP.IP), mdSInt(Context, IP.State, 32)}));
  } else {
    CxxHeader = Node({});
  }

  llvm::Metadata *GSCookie = Node({});
  if (EH.GSCookie) {
    const GSCookieInfo &GS = *EH.GSCookie;
    GSCookie =
        Node({Str(getExceptionParseStatusName(GS.ParseStatus)),
              mdSInt(Context, GS.CookieOffset, 32),
              mdUInt(Context, GS.HasExceptionHandler, 1),
              mdUInt(Context, GS.HasUnwindHandler, 1),
              mdUInt(Context, GS.HasAlignment, 1),
              mdSInt(Context, GS.AlignmentBaseOffset, 32),
              mdUInt(Context, GS.Alignment, 32), Str(hexBytes(GS.Payload))});
  }

  auto OptionalSInt = [&](const std::optional<int32_t> &Value) {
    return Value ? Node({mdSInt(Context, *Value, 32)}) : Node({});
  };
  llvm::Metadata *Registration = Node({});
  if (EH.Registration) {
    const RegistrationChainInfo &Chain = *EH.Registration;
    std::vector<llvm::Metadata *> TryLevelStores;
    TryLevelStores.reserve(Chain.TryLevelStores.size());
    for (const RegistrationTryLevelStore &Store : Chain.TryLevelStores)
      TryLevelStores.push_back(
          Node({mdUInt(Context, Store.StoreVA), mdUInt(Context, Store.EndVA),
                mdSInt(Context, Store.Level, 32)}));

    std::vector<llvm::Metadata *> Scopes;
    Scopes.reserve(Chain.Scopes.size());
    for (const RegistrationScopeRecord &Scope : Chain.Scopes)
      Scopes.push_back(Node({mdSInt(Context, Scope.EnclosingLevel, 32),
                             mdUInt(Context, Scope.FilterVA),
                             mdUInt(Context, Scope.HandlerVA),
                             mdUInt(Context, Scope.IsFinally, 1)}));

    Registration = Node(
        {mdUInt(Context, Chain.HandlerVA), mdUInt(Context, Chain.ScopeTableVA),
         OptionalSInt(Chain.TryLevelOffset), Node(TryLevelStores),
         OptionalSInt(Chain.SeededTryLevel),
         OptionalSInt(Chain.RegistrationOffset),
         mdUInt(Context, Chain.HasSecurityCookies, 1),
         mdSInt(Context, Chain.GSCookieOffset, 32),
         mdSInt(Context, Chain.GSCookieXOROffset, 32),
         mdSInt(Context, Chain.EHCookieOffset, 32),
         mdSInt(Context, Chain.EHCookieXOROffset, 32),
         mdUInt(Context, Chain.ScopeTableMagic, 32), Node(Scopes),
         mdUInt(Context, Chain.ChainInstallVA),
         mdUInt(Context, Chain.ChainRemoveVA)});
  }

  std::vector<llvm::Metadata *> Diagnostics;
  for (const std::string &Message : EH.Diagnostics)
    Diagnostics.push_back(Str(Message));

  llvm::Metadata *PrimaryFunctionIndex = Node({});
  if (EH.PrimaryFunctionIndex)
    PrimaryFunctionIndex = Node(
        {mdUInt(Context, static_cast<uint64_t>(*EH.PrimaryFunctionIndex))});
  llvm::Metadata *ChainedPrimaryRange = Node({});
  if (EH.ChainedPrimaryRange)
    ChainedPrimaryRange = Node({mdUInt(Context, EH.ChainedPrimaryRange->Begin),
                                mdUInt(Context, EH.ChainedPrimaryRange->End)});

  return Node({mdUInt(Context, SchemaVersion, 32),
               Str(getExceptionParseStatusName(EH.ParseStatus)),
               Str(getExceptionEncodingName(EH.Encoding)),
               mdUInt(Context, static_cast<uint8_t>(EH.Kind), 8),
               mdUInt(Context, EH.CodeRange.Begin),
               mdUInt(Context, EH.CodeRange.End),
               mdUInt(Context, EH.RuntimeFunctionRVA, 32),
               mdUInt(Context, EH.UnwindInfoRVA, 32),
               mdUInt(Context, EH.UnwindInfoVA),
               mdUInt(Context, EH.UnwindVersion, 8),
               mdUInt(Context, EH.UnwindFlags, 8),
               mdUInt(Context, EH.PrologueSize, 32),
               mdUInt(Context, EH.FrameRegister, 16),
               mdUInt(Context, EH.FrameOffset, 32),
               mdUInt(Context, EH.PackedUnwindData, 32),
               Str(getExceptionPersonalityName(EH.Personality)),
               Str(EH.PersonalityName),
               mdUInt(Context, EH.PersonalityVA),
               mdUInt(Context, EH.HandlerDataVA),
               Str(hexBytes(EH.NativeUnwindBytes)),
               Node(UnwindOps),
               Node(Epilogs),
               Node(SEHScopes),
               CxxHeader,
               Node(CxxUnwind),
               Node(CxxTry),
               Node(CxxIP),
               GSCookie,
               PrimaryFunctionIndex,
               ChainedPrimaryRange,
               mdUInt(Context, EH.ChainedUnwindInfoRVA, 32),
               Node(Diagnostics),
               mdUInt(Context,
                      classifyWindowsEHNativeSource(
                          EH, TargetArch, TargetFormat,
                          WindowsEHNativeCapability::OutputPatch)
                          .canPatchOutput(),
                      1),
               Registration});
}

llvm::MDNode *getCanonicalFunctionMetadata(llvm::LLVMContext &Context,
                                           const ExceptionFunction &EH) {
  return getCanonicalFunctionMetadata(Context, EH, Arch::Unknown,
                                      BinaryFormat::Unknown);
}

} // namespace neverd::windows_eh_md
