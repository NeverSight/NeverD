//===- MedLLVMExceptionMetadata.cpp - Exception metadata -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Serialization of normalized exception information into LLVM metadata.
///
//===----------------------------------------------------------------------===//

#include "MedLLVMEHHelpers.h"

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Metadata.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neverd {

using med_llvm_eh::hexBytes;
using med_llvm_eh::mdSInt;
using med_llvm_eh::mdUInt;

void MedLLVMEmitter::emitExceptionMetadata(const MedFunc &Func,
                                           llvm::Function &LLVMFunc) {
  if (!Func.ExceptionMetadata)
    return;
  const ExceptionFunction &EH = *Func.ExceptionMetadata;
  auto Str = [&](llvm::StringRef Value) -> llvm::Metadata * {
    return llvm::MDString::get(*Ctx, Value);
  };
  auto Node = [&](const std::vector<llvm::Metadata *> &Values) {
    return llvm::MDNode::get(*Ctx, Values);
  };

  auto UnwindOp = [&](const UnwindOperation &Op) -> llvm::Metadata * {
    return Node({mdUInt(*Ctx, static_cast<uint8_t>(Op.Kind), 8),
                 mdUInt(*Ctx, Op.CodeOffset, 32), mdUInt(*Ctx, Op.OpInfo, 8),
                 mdUInt(*Ctx, Op.SlotCount, 8), mdUInt(*Ctx, Op.Register, 16),
                 mdUInt(*Ctx, Op.StackOffset), Str(hexBytes(Op.OperandBytes)),
                 mdUInt(*Ctx, static_cast<uint8_t>(Op.RegisterClass), 8),
                 mdUInt(*Ctx, Op.RegisterMask, 32),
                 mdUInt(*Ctx, Op.InstructionSize, 8)});
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
    Epilogs.push_back(
        Node({mdSInt(*Ctx, Epilog.StartOffset), mdUInt(*Ctx, Epilog.Flags, 8),
              mdUInt(*Ctx, Epilog.FirstOperationOffset, 32),
              mdUInt(*Ctx, Epilog.LastInstructionOffset, 32), Node(Ops)}));
  }

  std::vector<llvm::Metadata *> SEHScopes;
  if (EH.SEH) {
    SEHScopes.reserve(EH.SEH->Scopes.size());
    for (const SEHScopeRecord &Scope : EH.SEH->Scopes)
      SEHScopes.push_back(Node(
          {mdUInt(*Ctx, Scope.GuardedRange.Begin),
           mdUInt(*Ctx, Scope.GuardedRange.End),
           mdUInt(*Ctx, static_cast<uint8_t>(Scope.Kind), 8),
           mdUInt(*Ctx, Scope.FilterOrFinallyVA), mdUInt(*Ctx, Scope.HandlerVA),
           mdUInt(*Ctx, Scope.ContinuationVA),
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
      CxxSpecTypes.push_back(Node({mdUInt(*Ctx, Spec.Adjectives, 32),
                                   mdUInt(*Ctx, Spec.TypeDescriptorVA)}));
    CxxHeader = Node(
        {mdUInt(*Ctx, static_cast<uint8_t>(Cxx.NativeEncoding), 8),
         mdUInt(*Ctx, Cxx.Magic, 32), mdUInt(*Ctx, Cxx.Flags, 32),
         mdUInt(*Ctx, Cxx.MaxState, 32), mdSInt(*Ctx, Cxx.UnwindHelpOffset, 32),
         mdUInt(*Ctx, Cxx.ESTypeListVA), mdUInt(*Ctx, Cxx.BBTFlags, 32),
         mdUInt(*Ctx, Cxx.FrameOffset, 32), mdUInt(*Ctx, Cxx.IsCatchFunclet, 1),
         mdUInt(*Ctx, Cxx.IsSeparated, 1), mdUInt(*Ctx, Cxx.IsSynchronous, 1),
         mdUInt(*Ctx, Cxx.IsNoExcept, 1),
         mdUInt(*Ctx, static_cast<uint8_t>(Cxx.Version), 8),
         mdUInt(*Ctx, Cxx.HasDynamicStackAlignment, 1), Node(CxxSpecTypes)});
    for (const CxxUnwindAction &Action : Cxx.UnwindMap)
      CxxUnwind.push_back(
          Node({mdSInt(*Ctx, Action.ToState, 32), mdUInt(*Ctx, Action.ActionVA),
                mdUInt(*Ctx, static_cast<uint8_t>(Action.Kind), 8),
                mdSInt(*Ctx, Action.ObjectOffset, 32)}));
    for (const CxxTryBlock &Try : Cxx.TryBlocks) {
      std::vector<llvm::Metadata *> Catches;
      for (const CxxCatchHandler &Catch : Try.Handlers) {
        std::vector<llvm::Metadata *> Continuations;
        for (va_t Address : Catch.ContinuationVAs)
          Continuations.push_back(mdUInt(*Ctx, Address));
        Catches.push_back(Node({mdUInt(*Ctx, Catch.Adjectives, 32),
                                mdUInt(*Ctx, Catch.TypeDescriptorVA),
                                mdSInt(*Ctx, Catch.CatchObjectOffset, 32),
                                mdUInt(*Ctx, Catch.HandlerVA),
                                mdSInt(*Ctx, Catch.ParentFrameOffset, 32),
                                Node(Continuations)}));
      }
      CxxTry.push_back(
          Node({mdSInt(*Ctx, Try.TryLow, 32), mdSInt(*Ctx, Try.TryHigh, 32),
                mdSInt(*Ctx, Try.CatchHigh, 32), Node(Catches)}));
    }
    for (const CxxIPState &IP : Cxx.IPMap)
      CxxIP.push_back(Node({mdUInt(*Ctx, IP.IP), mdSInt(*Ctx, IP.State, 32)}));
  } else {
    CxxHeader = Node({});
  }

  llvm::Metadata *GSCookie = Node({});
  if (EH.GSCookie) {
    const GSCookieInfo &GS = *EH.GSCookie;
    GSCookie = Node(
        {Str(getExceptionParseStatusName(GS.ParseStatus)),
         mdSInt(*Ctx, GS.CookieOffset, 32),
         mdUInt(*Ctx, GS.HasExceptionHandler, 1),
         mdUInt(*Ctx, GS.HasUnwindHandler, 1), mdUInt(*Ctx, GS.HasAlignment, 1),
         mdSInt(*Ctx, GS.AlignmentBaseOffset, 32),
         mdUInt(*Ctx, GS.Alignment, 32), Str(hexBytes(GS.Payload))});
  }

  std::vector<llvm::Metadata *> Diagnostics;
  for (const std::string &Message : EH.Diagnostics)
    Diagnostics.push_back(Str(Message));

  llvm::Metadata *PrimaryFunctionIndex = Node({});
  if (EH.PrimaryFunctionIndex)
    PrimaryFunctionIndex =
        Node({mdUInt(*Ctx, static_cast<uint64_t>(*EH.PrimaryFunctionIndex))});
  llvm::Metadata *ChainedPrimaryRange = Node({});
  if (EH.ChainedPrimaryRange)
    ChainedPrimaryRange = Node({mdUInt(*Ctx, EH.ChainedPrimaryRange->Begin),
                                mdUInt(*Ctx, EH.ChainedPrimaryRange->End)});

  llvm::MDNode *Payload =
      Node({mdUInt(*Ctx, windows_eh_md::SchemaVersion, 32),
            Str(getExceptionParseStatusName(EH.ParseStatus)),
            Str(getExceptionEncodingName(EH.Encoding)),
            mdUInt(*Ctx, static_cast<uint8_t>(EH.Kind), 8),
            mdUInt(*Ctx, EH.CodeRange.Begin),
            mdUInt(*Ctx, EH.CodeRange.End),
            mdUInt(*Ctx, EH.RuntimeFunctionRVA, 32),
            mdUInt(*Ctx, EH.UnwindInfoRVA, 32),
            mdUInt(*Ctx, EH.UnwindInfoVA),
            mdUInt(*Ctx, EH.UnwindVersion, 8),
            mdUInt(*Ctx, EH.UnwindFlags, 8),
            mdUInt(*Ctx, EH.PrologueSize, 32),
            mdUInt(*Ctx, EH.FrameRegister, 16),
            mdUInt(*Ctx, EH.FrameOffset, 32),
            mdUInt(*Ctx, EH.PackedUnwindData, 32),
            Str(getExceptionPersonalityName(EH.Personality)),
            Str(EH.PersonalityName),
            mdUInt(*Ctx, EH.PersonalityVA),
            mdUInt(*Ctx, EH.HandlerDataVA),
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
            mdUInt(*Ctx, EH.ChainedUnwindInfoRVA, 32),
            Node(Diagnostics),
            mdUInt(*Ctx, EH.canRegenerateLanguageMetadata(), 1)});
  LLVMFunc.setMetadata(windows_eh_md::FunctionAttachment, Payload);
  llvm::NamedMDNode *Table =
      Mod->getOrInsertNamedMetadata(windows_eh_md::FunctionTable);
  Table->addOperand(Node({llvm::ValueAsMetadata::get(&LLVMFunc), Payload}));
}

} // namespace neverd
