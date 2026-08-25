//===- MedLLVMEHHelpers.h - MedLLVM EH helpers ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal metadata helpers shared by the MedLLVM EH translation units.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_LLVM_MEDLLVMEHHELPERS_H
#define NEVERD_LIB_BACKEND_LLVM_MEDLLVMEHHELPERS_H

#include "neverd/Common.h"
#include "neverd/backend/llvm/LanguageEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/BinaryRewrite.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace neverd::med_llvm_eh {

/// Emit a codegen-neutral anchor for one native Windows EH edge.  The
/// sideeffect intrinsic survives LLVM optimization while its unknown numeric
/// bundle remains available to the patch verifier; target lowering discards
/// the intrinsic itself.
inline llvm::CallInst *emitWindowsEHProvenanceAnchor(
    llvm::IRBuilder<> &Builder, windows_eh_md::NativeProvenanceModel Model,
    windows_eh_md::NativeProvenanceRole Role, va_t FunctionVA, va_t SourceVA,
    uint32_t Region, uint32_t Clause, llvm::Value *FuncletToken = nullptr,
    va_t AuxVA = 0, uint32_t Flags = 0) {
  llvm::Module *Module = Builder.GetInsertBlock()->getModule();
  llvm::Function *SideEffect = llvm::Intrinsic::getOrInsertDeclaration(
      Module, llvm::Intrinsic::sideeffect);
  llvm::LLVMContext &Context = Module->getContext();
  llvm::SmallVector<llvm::Value *, windows_eh_md::ProvenanceOperandCount>
      Inputs{
          llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context),
                                 windows_eh_md::ProvenanceSchemaVersion),
          llvm::ConstantInt::get(llvm::Type::getInt8Ty(Context),
                                 static_cast<unsigned>(Model)),
          llvm::ConstantInt::get(llvm::Type::getInt8Ty(Context),
                                 static_cast<unsigned>(Role)),
          llvm::ConstantInt::get(llvm::Type::getInt64Ty(Context), FunctionVA),
          llvm::ConstantInt::get(llvm::Type::getInt64Ty(Context), SourceVA),
          llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), Region),
          llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), Clause),
          llvm::ConstantInt::get(llvm::Type::getInt64Ty(Context), AuxVA),
          llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), Flags),
      };
  llvm::SmallVector<llvm::OperandBundleDef, 2> Bundles;
  Bundles.emplace_back(windows_eh_md::ProvenanceBundle.str(), Inputs);
  if (FuncletToken) {
    llvm::SmallVector<llvm::Value *, 1> FuncletInputs{FuncletToken};
    Bundles.emplace_back("funclet", FuncletInputs);
  }
  return Builder.CreateCall(SideEffect->getFunctionType(), SideEffect, {},
                            Bundles);
}

inline llvm::Metadata *mdUInt(llvm::LLVMContext &Ctx, uint64_t Value,
                              unsigned Bits = 64) {
  return llvm::ConstantAsMetadata::get(
      llvm::ConstantInt::get(llvm::IntegerType::get(Ctx, Bits), Value));
}

inline llvm::Metadata *mdSInt(llvm::LLVMContext &Ctx, int64_t Value,
                              unsigned Bits = 64) {
  return llvm::ConstantAsMetadata::get(
      llvm::ConstantInt::getSigned(llvm::IntegerType::get(Ctx, Bits), Value));
}

/// Attach one source-issued Windows EH semantic token to a funclet pad for
/// WinEH preparation.  Reject invalid tokens and pre-existing attachments so
/// the producer cannot silently replace an earlier semantic identity.
inline bool attachRewriteWinEHSemanticToken(
    llvm::Instruction &Pad,
    const llvm::mc_rewrite::RewriteWinEHSemanticToken &Token) {
  if ((!llvm::isa<llvm::CatchPadInst>(&Pad) &&
       !llvm::isa<llvm::CleanupPadInst>(&Pad)) ||
      Pad.getMetadata(llvm::mc_rewrite::RewriteWinEHSemanticAttachment))
    return false;

  if (Token.Kind != llvm::mc_rewrite::RewriteWinEHSemanticKind::SEHScope &&
      Token.Kind != llvm::mc_rewrite::RewriteWinEHSemanticKind::CxxCatch)
    return false;

  bool HasDigest = false;
  for (uint64_t Word : Token.Digest)
    HasDigest |= Word != 0;
  if (!HasDigest)
    return false;

  llvm::LLVMContext &Context = Pad.getContext();
  llvm::SmallVector<llvm::Metadata *,
                    llvm::mc_rewrite::RewriteWinEHSemanticOperandCount>
      Operands{
          mdUInt(Context, llvm::mc_rewrite::RewriteWinEHSemanticSchemaVersion,
                 32),
          mdUInt(Context, static_cast<uint8_t>(Token.Kind), 8),
          mdUInt(Context, Token.Region, 32),
          mdUInt(Context, Token.Clause, 32),
      };
  for (uint64_t Word : Token.Digest)
    Operands.push_back(mdUInt(Context, Word, 64));

  Pad.setMetadata(llvm::mc_rewrite::RewriteWinEHSemanticAttachment,
                  llvm::MDNode::get(Context, Operands));
  return true;
}

inline std::string hexBytes(const std::vector<uint8_t> &Bytes) {
  static constexpr char Digits[] = "0123456789abcdef";
  std::string Result;
  Result.reserve(Bytes.size() * 2);
  for (uint8_t Byte : Bytes) {
    Result.push_back(Digits[Byte >> 4]);
    Result.push_back(Digits[Byte & 0x0f]);
  }
  return Result;
}

using SourceCallAddressMap = std::map<const llvm::CallInst *, va_t>;

/// Read every transient source-call marker in \p Function and require it to
/// equal \p TrackedCalls exactly.  Pointer identity, address values, missing
/// entries, and entries owned by another function all participate in the
/// comparison.
inline std::optional<SourceCallAddressMap>
collectExactSourceCallAddresses(const llvm::Function &Function,
                                const SourceCallAddressMap &TrackedCalls) {
  SourceCallAddressMap MarkedCalls;
  for (const llvm::BasicBlock &Block : Function) {
    for (const llvm::Instruction &Instruction : Block) {
      const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction);
      if (!Call)
        continue;
      const llvm::MDNode *Marker =
          Call->getMetadata(language_eh_md::InternalSourceCallAttachment);
      if (!Marker)
        continue;
      if (Marker->getNumOperands() != 1)
        return std::nullopt;
      const auto *Metadata = llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(
          Marker->getOperand(0).get());
      const auto *Address =
          Metadata ? llvm::dyn_cast<llvm::ConstantInt>(Metadata->getValue())
                   : nullptr;
      if (!Address || Address->getBitWidth() != 64 ||
          !MarkedCalls.emplace(Call, Address->getZExtValue()).second)
        return std::nullopt;
    }
  }
  if (MarkedCalls != TrackedCalls)
    return std::nullopt;
  return MarkedCalls;
}

enum class I32ModuleFlagState { Absent, Compatible, Conflict };

/// Classify a scalar i32 module flag without using Module's unchecked flag
/// accessors.  Native lowering runs before final module verification, so a
/// malformed or duplicate flag must fail closed instead of reaching the
/// accessor casts or silently keeping an incompatible value.
inline I32ModuleFlagState
classifyI32ModuleFlag(const llvm::Module &Module, llvm::StringRef Name,
                      llvm::Module::ModFlagBehavior ExpectedBehavior,
                      uint32_t ExpectedValue) {
  const llvm::NamedMDNode *Flags = Module.getModuleFlagsMetadata();
  if (!Flags)
    return I32ModuleFlagState::Absent;

  bool Found = false;
  for (const llvm::MDNode *Flag : Flags->operands()) {
    if (!Flag || Flag->getNumOperands() != 3)
      return I32ModuleFlagState::Conflict;

    llvm::Module::ModFlagBehavior Behavior;
    if (!llvm::Module::isValidModFlagBehavior(Flag->getOperand(0), Behavior))
      return I32ModuleFlagState::Conflict;
    const auto *Key =
        llvm::dyn_cast_or_null<llvm::MDString>(Flag->getOperand(1).get());
    if (!Key)
      return I32ModuleFlagState::Conflict;
    if (Key->getString() != Name)
      continue;
    if (Found || Behavior != ExpectedBehavior)
      return I32ModuleFlagState::Conflict;
    Found = true;

    const auto *ValueMetadata =
        llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(
            Flag->getOperand(2).get());
    const auto *Value =
        ValueMetadata
            ? llvm::dyn_cast<llvm::ConstantInt>(ValueMetadata->getValue())
            : nullptr;
    if (!Value || Value->getBitWidth() != 32 ||
        Value->getZExtValue() != ExpectedValue)
      return I32ModuleFlagState::Conflict;
  }
  return Found ? I32ModuleFlagState::Compatible : I32ModuleFlagState::Absent;
}

/// True when \p Name is unused or already denotes the declaration that
/// getOrInsertFunction would otherwise create.  An alias, definition, local
/// symbol, or differently typed function cannot stand in for an external ABI
/// entry point.
inline bool
canMaterializeExternalFunctionDeclaration(const llvm::Module &Module,
                                          llvm::StringRef Name,
                                          const llvm::FunctionType *Type) {
  const llvm::GlobalValue *Named = Module.getNamedValue(Name);
  if (!Named)
    return true;
  const auto *Function = llvm::dyn_cast<llvm::Function>(Named);
  return Function && Function->isDeclaration() &&
         Function->getLinkage() == llvm::GlobalValue::ExternalLinkage &&
         Function->getVisibility() == llvm::GlobalValue::DefaultVisibility &&
         Function->getCallingConv() == llvm::CallingConv::C &&
         Function->getAddressSpace() == 0 &&
         Function->getFunctionType() == Type;
}

/// True when \p Name is unused or already denotes the exact external data
/// declaration that native EH lowering needs.
inline bool canMaterializeExternalDataDeclaration(const llvm::Module &Module,
                                                  llvm::StringRef Name,
                                                  const llvm::Type *Type,
                                                  bool IsConstant) {
  const llvm::GlobalValue *Named = Module.getNamedValue(Name);
  if (!Named)
    return true;
  const auto *Variable = llvm::dyn_cast<llvm::GlobalVariable>(Named);
  return Variable && Variable->isDeclaration() &&
         Variable->getLinkage() == llvm::GlobalValue::ExternalLinkage &&
         Variable->getVisibility() == llvm::GlobalValue::DefaultVisibility &&
         Variable->getValueType() == Type &&
         Variable->isConstant() == IsConstant &&
         Variable->getAddressSpace() == 0 && !Variable->isThreadLocal();
}

} // namespace neverd::med_llvm_eh

#endif // NEVERD_LIB_BACKEND_LLVM_MEDLLVMEHHELPERS_H
