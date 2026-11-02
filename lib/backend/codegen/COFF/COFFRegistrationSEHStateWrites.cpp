//===- COFFRegistrationSEHStateWrites.cpp - Native state stores --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticates every generated callback state write and its exact bytes.

#include "COFFRegistrationSEHStateWrites.h"

#include "../X86/CompiledSEHStateWrites.h"
#include "COFFRegistrationIRProof.h"
#include "COFFRegistrationTableProof.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
#include "neverd/backend/llvm/X86/X86RegistrationSEHJump.h"

#include "llvm/IR/IntrinsicsX86.h"

#include <map>
#include <set>

namespace neverd {

std::optional<uint8_t>
getCOFFRegistrationSEHStateWriteRegister(llvm::ArrayRef<uint8_t> Bytes,
                                         int32_t Offset, int32_t State) {
  if (Bytes.size() < 6 || Bytes[0] != 0xc7)
    return std::nullopt;
  const uint8_t Mode = Bytes[1] >> 6;
  uint8_t Base = Bytes[1] & 7;
  if ((Bytes[1] & 0x38) || Mode == 3)
    return std::nullopt;
  size_t Cursor = 2;
  if (Base == 4) {
    const uint8_t SIB = Bytes[Cursor++];
    if ((SIB & 0x38) != 0x20)
      return std::nullopt;
    Base = SIB & 7;
  }
  if (Mode == 0 && Base == 5)
    return std::nullopt;
  const size_t DisplacementBytes = Mode == 2 ? 4 : Mode == 1 ? 1 : 0;
  if (Bytes.size() != Cursor + DisplacementBytes + 4)
    return std::nullopt;
  const int32_t Displacement = Mode == 2
                                   ? readLE<int32_t>(Bytes.data() + Cursor)
                               : Mode == 1 ? int8_t(Bytes[Cursor])
                                           : 0;
  Cursor += DisplacementBytes;
  if (Displacement != Offset || readLE<int32_t>(Bytes.data() + Cursor) != State)
    return std::nullopt;
  return Base;
}

llvm::Error
validateCOFFRegistrationSEHStateWrites(const llvm::Function &Parent,
                                       const ExceptionFunction &Source,
                                       const CompiledImage &Compiled) {
  using namespace coff_registration;
  if (!Source.Registration || !Source.Registration->SeededTryLevel)
    return rejectIR("callback state writes have no source registration");
  auto CallbackMap = callbacks(Parent, Source);
  if (!CallbackMap)
    return CallbackMap.takeError();
  std::set<const llvm::Function *> Functions{&Parent};
  for (const auto &[Identity, Function] : *CallbackMap)
    Functions.insert(Function);
#ifdef LLVM_NEVERD_X86_REGISTRATION_SEH_STATE_WRITES
  if (!Compiled.Success || !validateCompiledX86SEHStateWrites(Compiled))
    return rejectIR("compiler callback state-write receipts are invalid");
  using WriteKey = std::pair<std::string, uint64_t>;
  std::map<WriteKey, const CompiledWinX86SEHStateWrite *> Receipts;
  for (const auto &Write : Compiled.WinX86SEHStateWrites)
    if (Write.ParentFrame.SourceFunction == Parent.getName() &&
        !Receipts.emplace(WriteKey{Write.SourceFunction, Write.WriteID}, &Write)
             .second)
      return rejectIR("callback state-write receipt identity is duplicated");
  std::set<const CompiledWinX86SEHStateWrite *> Used;
#endif
  size_t Work = 0;
  for (const auto *Function : Functions)
    for (const auto &Block : *Function)
      for (const auto &I : Block) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return rejectIR("callback state-write validation exceeds its budget");
        const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
        if (!Call || !Call->getCalledFunction() ||
            Call->getCalledFunction()->getName() != "llvm.x86.seh.setstate")
          continue;
#ifndef LLVM_NEVERD_X86_REGISTRATION_SEH_STATE_WRITES
        return rejectIR("LLVM lacks physical callback state-write receipts");
#else
        const auto *Frame =
            getX86RegistrationSEHJumpParentFrame(*Function, Parent);
        if (Function == &Parent || !llvm::isa<llvm::CallInst>(Call) ||
            Call->getIntrinsicID() != llvm::Intrinsic::x86_seh_setstate ||
            Call->arg_size() != 8 || Call->getNumOperandBundles() ||
            Call->getCallingConv() != llvm::CallingConv::C ||
            !Call->getAttributes().isEmpty() || !Call->use_empty() ||
            llvm::cast<llvm::CallInst>(Call)->isTailCall() ||
            Call->getArgOperand(0) != &Parent || !Frame ||
            Call->getArgOperand(1) != Frame)
          return rejectIR("physical state write changed its callback ABI");
        const auto ID = integer(Call->getArgOperand(7), 64);
        const auto LevelBits = integer(Call->getArgOperand(2), 32);
        if (!ID || !*ID || !LevelBits)
          return rejectIR("physical state write has no exact source identity");
        const auto Found = Receipts.find({Function->getName().str(), *ID});
        if (Found == Receipts.end() || !Used.insert(Found->second).second)
          return rejectIR(
              "physical state write has no unique compiler receipt");
        const auto &Write = *Found->second;
        auto ParentVA = ownerVA(Parent, Compiled);
        if (!ParentVA)
          return ParentVA.takeError();
        auto CallbackVA = ownerVA(*Function, Compiled);
        if (!CallbackVA)
          return CallbackVA.takeError();
        if (Write.OwnerVA != *CallbackVA ||
            Write.ParentFrame.OwnerVA != *ParentVA)
          return rejectIR("physical state write changed its generated owners");
        const int32_t Level = int32_t(uint32_t(*LevelBits));
        std::array<uint64_t, 4> Digest{};
        if (Level >= 0) {
          const auto Token = windows_eh_semantics::getSEHScopeSemanticToken(
              Source, Arch::X86, uint32_t(Level));
          if (!Token || !Write.Semantic || *Write.Semantic != *Token)
            return rejectIR("physical state write changed its source scope");
          Digest = Token->Digest;
        } else if (Level != *Source.Registration->SeededTryLevel ||
                   Write.Semantic || Write.GeneratedState != Level) {
          return rejectIR("physical state write changed its inactive sentinel");
        }
        for (unsigned J = 0; J != Digest.size(); ++J)
          if (integer(Call->getArgOperand(3 + J), 64) != Digest[J])
            return rejectIR("physical state write changed its source digest");
        const uint64_t Size = Write.EndVA - Write.BeginVA;
        const auto *Section =
            sectionAt(Compiled, Write.BeginVA, Size,
                      llvm::mc_rewrite::RewriteSectionKind::Code);
        if (!Section)
          return rejectIR("physical state write has no final code storage");
        const auto Bytes = llvm::ArrayRef<uint8_t>(Compiled.Bytes)
                               .slice(Write.BeginVA - Compiled.BaseVA, Size);
        const int64_t Offset = int64_t(Write.ParentFrame.RegistrationOffset) +
                               Write.ParentFrame.StateOffset;
        if (Offset < INT32_MIN || Offset > INT32_MAX ||
            getCOFFRegistrationSEHStateWriteRegister(Bytes, int32_t(Offset),
                                                     Write.GeneratedState) !=
                Write.AddressRegister)
          return rejectIR(
              "physical state-write bytes changed their MOV contract");
#endif
      }
#ifdef LLVM_NEVERD_X86_REGISTRATION_SEH_STATE_WRITES
  if (Used.size() != Receipts.size())
    return rejectIR(
        "compiler state write has no authenticated callback operation");
#endif
  return llvm::Error::success();
}

} // namespace neverd
