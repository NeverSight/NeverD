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

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHMetadataEncoder.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Metadata.h"

namespace neverd {

void MedLLVMEmitter::emitExceptionMetadata(const MedFunc &Func,
                                           llvm::Function &LLVMFunc) {
  if (!Func.ExceptionMetadata) {
    exception_rewrite::setContract(
        LLVMFunc, exception_rewrite::SourceState::Absent,
        exception_rewrite::LoweringState::NotRequired);
    return;
  }
  const ExceptionFunction &EH = *Func.ExceptionMetadata;

  exception_rewrite::SourceState Source =
      exception_rewrite::SourceState::Complete;
  switch (EH.ParseStatus) {
  case ExceptionParseStatus::Complete:
    break;
  case ExceptionParseStatus::Partial:
    Source = exception_rewrite::SourceState::Partial;
    break;
  case ExceptionParseStatus::Malformed:
    Source = exception_rewrite::SourceState::Malformed;
    break;
  }
  // Missing means native IR lowering was required and has not been applied.
  // Known-unsupported Windows shapes (C++ dtors, out-of-line funclets,
  // AArch64 SEH callbacks) stay metadata-only so `--llvm` opt is not
  // `input-invalid`. Empty/malformed tables and Itanium refusals stay
  // Missing. Eligible emitters overwrite this with Complete after they finish.
  exception_rewrite::LoweringState Lowering =
      exception_rewrite::LoweringState::NotRequired;
  if (EH.hasLanguageTable()) {
    const WindowsEHNativeSourceClassification IR =
        classifyWindowsEHNativeSource(EH, TargetArch, TargetFormat,
                                      WindowsEHNativeCapability::IRLowering);
    if (!isIntentionalMetadataOnlyNativeIR(IR.Reason))
      Lowering = exception_rewrite::LoweringState::Missing;
  }
  exception_rewrite::setContract(LLVMFunc, Source, Lowering);
  // The source described a native frame even when it carried no language
  // table.  Ask the target backend for its native CFI so the object-format
  // installer has something target-correct to register for the new frame.
  LLVMFunc.setUWTableKind(llvm::UWTableKind::Default);

  llvm::MDNode *Payload = windows_eh_md::getCanonicalFunctionMetadata(
      *Ctx, EH, TargetArch, TargetFormat);
  LLVMFunc.setMetadata(windows_eh_md::FunctionAttachment, Payload);
  llvm::NamedMDNode *Table =
      Mod->getOrInsertNamedMetadata(windows_eh_md::FunctionTable);
  Table->addOperand(llvm::MDNode::get(
      *Ctx, {llvm::ValueAsMetadata::get(&LLVMFunc), Payload}));
}

} // namespace neverd
