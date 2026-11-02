//===- LLVMCIntrinsicRenderARM.cpp - ARM inline-asm rendering ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM/AArch64-specific inline-assembly-to-C rendering: asm-mnemonic lookup
/// table and GCC __asm__ volatile / ACLE translation.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/render/AArch64/AArch64AddressMemoryC.h"
#include "neverd/backend/c/render/AArch64/AArch64CPAC.h"
#include "neverd/backend/c/render/AArch64/AArch64PAuthC.h"
#include "neverd/backend/c/render/AArch64/AArch64PAuthLoadC.h"
#include "neverd/backend/c/render/AArch64/AArch64PointerC.h"
#include "neverd/backend/c/render/AArch64/AArch64StructureMemoryC.h"
#include "neverd/backend/c/render/AArch64/AArch64WritebackMemoryC.h"
#include "neverd/backend/c/render/ARM/ArmCoprocessorAcle.h"
#include "neverd/backend/c/render/LLVMC/LLVMCIntrinsicRender.h"
#include "neverd/backend/llvm/AArch64StructureMemoryAsm.h"
#include "neverd/ir/intrinsics/IntrinsicInstructionText.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/AtomicOrdering.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>

namespace neverd {

namespace {

static const AsmToCEntry ARMAsmTable[] = {
#define M(a, c) {a, c},
#include "LLVMCAsmToCARM.inc"
#undef M
};

} // anonymous namespace

bool validateAArch64AddressMemoryCall(Arch TheArch,
                                      const llvm::CallInst &Call) {
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!Asm)
    return false;
  const auto *Machine =
      llvm::find_if(kAddressMemoryMachines, [&](const auto &M) {
        return Asm->getAsmString() == M.Asm;
      });
  if (Machine == std::end(kAddressMemoryMachines))
    return false;
  const bool Store = Machine->Store;
  const bool Vector = Machine->Vector;
  const auto ValueType = [&](llvm::Type *Type) {
    if (!Vector)
      return Type->isIntegerTy(kAddressMemoryPointerBits);
    const auto *VectorType = llvm::dyn_cast<llvm::FixedVectorType>(Type);
    return VectorType &&
           VectorType->getNumElements() ==
               kAddressMemoryVectorBytes / kAddressMemoryPointerBytes &&
           VectorType->getElementType()->isIntegerTy(kAddressMemoryPointerBits);
  };
  const unsigned Count =
      Store ? kAddressStoreOperandCount : kAddressLoadOperandCount;
  if (TheArch != Arch::AArch64 ||
      Asm->getConstraintString() != Machine->Constraints ||
      !Asm->hasSideEffects() || Asm->isAlignStack() || Asm->canThrow() ||
      Asm->getDialect() != llvm::InlineAsm::AD_ATT ||
      Call.arg_size() != Count || Call.getFunctionType()->isVarArg() ||
      Call.getNumOperandBundles() ||
      Call.getCallingConv() != llvm::CallingConv::C || Call.doesNotReturn() ||
      Call.hasFnAttr(llvm::Attribute::Speculatable) ||
      Call.getMemoryEffects() != llvm::MemoryEffects::unknown() ||
      (Store ? !Call.getType()->isVoidTy() : !ValueType(Call.getType())) ||
      !Call.getArgOperand(kAddressMemoryBaseOperand)
           ->getType()
           ->isIntegerTy(kAddressMemoryPointerBits) ||
      !Call.getArgOperand(kAddressMemoryIndexOperand)
           ->getType()
           ->isIntegerTy(kAddressMemoryPointerBits) ||
      !Call.getArgOperand(kAddressMemoryWordOperand)
           ->getType()
           ->isIntegerTy(kAddressMemoryWordBytes * CHAR_BIT))
    throw std::invalid_argument(kAddressMemoryMalformed);
  if (Store &&
      !ValueType(Call.getArgOperand(kAddressMemoryDataOperand)->getType()))
    throw std::invalid_argument(kAddressMemoryMalformed);
  const auto *Word = llvm::dyn_cast<llvm::ConstantInt>(
      Call.getArgOperand(kAddressMemoryWordOperand));
  const auto Instruction =
      Word ? aarch64AddressMemoryInstruction(Word->getZExtValue())
           : std::nullopt;
  const auto *Index = llvm::dyn_cast<llvm::ConstantInt>(
      Call.getArgOperand(kAddressMemoryIndexOperand));
  if (!Instruction || Instruction->isLoad() == Store ||
      Instruction->isVector() != Vector ||
      Instruction->stackBase() != Machine->StackBase ||
      (!Machine->StackBase &&
       Instruction->base() != kAddressMemoryScratchBase) ||
      (Instruction->hasRegisterIndex()
           ? Instruction->index() != kAddressMemoryScratchIndex
           : !Index || !Index->isZero()) ||
      Instruction->destination() != kAddressMemoryScratchValue)
    throw std::invalid_argument(kAddressMemoryMalformed);
  return true;
}

std::optional<InlineAsmRender>
renderAArch64AddressMemoryCall(Arch TheArch, const llvm::CallInst &Call,
                               const std::string &ResultName, bool ResultLive,
                               const std::vector<std::string> &Args) {
  if (!validateAArch64AddressMemoryCall(TheArch, Call))
    return std::nullopt;
  const bool Store = Call.getType()->isVoidTy();
  if (Store && (ResultLive || !ResultName.empty()))
    throw std::invalid_argument(kAddressMemoryMalformed);
  if (Args.size() !=
      (Store ? kAddressStoreOperandCount : kAddressLoadOperandCount))
    throw std::invalid_argument(kAddressMemoryMalformed);
  const auto *Word = llvm::cast<llvm::ConstantInt>(
      Call.getArgOperand(kAddressMemoryWordOperand));
  const auto Instruction =
      *aarch64AddressMemoryInstruction(Word->getZExtValue());
  std::string Data = Store ? Args[kAddressMemoryDataOperand] : std::string{};
  if (Store && Instruction.isVector())
    Data = llvm::formatv(kAddressVectorToBitsC, Data).str();
  const auto Value =
      aarch64AddressMemoryC(Instruction, Args[kAddressMemoryBaseOperand],
                            Args[kAddressMemoryIndexOperand], Data);
  const auto Result = !Store && Instruction.isVector()
                          ? llvm::formatv(kAddressVectorFromBitsC, Value).str()
                          : Value;
  return InlineAsmRender{
      ResultLive && !ResultName.empty()
          ? llvm::formatv(kAArch64PointerCAssignment, ResultName, Result).str()
          : llvm::formatv(kAArch64PointerCDiscardStatement, Value).str(),
      false};
}

namespace {
std::optional<AArch64WritebackMemoryInstruction>
validatedAArch64WritebackMemoryCall(Arch TheArch, const llvm::CallInst &Call) {
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!Asm)
    return std::nullopt;
  const auto &Text = Asm->getAsmString();
  if (llvm::StringRef(Text).starts_with(kStructureAsmPrefix)) {
    const auto *Word = Call.arg_empty()
                           ? nullptr
                           : llvm::dyn_cast<llvm::ConstantInt>(
                                 Call.getArgOperand(Call.arg_size() - 1));
    const auto I =
        Word && Word->getBitWidth() == kAddressMemoryWordBytes * CHAR_BIT
            ? aarch64StructureMemoryInstruction(Word->getZExtValue())
            : std::nullopt;
    if (!I)
      throw std::invalid_argument(kWritebackMalformed);
    const auto Plan = aarch64StructureMemoryAsm(*I);
    const auto Vector = [](llvm::Type *T) {
      const auto *V = llvm::dyn_cast<llvm::FixedVectorType>(T);
      return V &&
             V->getNumElements() ==
                 kAddressMemoryVectorBytes / kAddressMemoryPointerBytes &&
             V->getElementType()->isIntegerTy(kAddressMemoryPointerBits);
    };
    bool ResultValid = Call.getType()->isIntegerTy(kAddressMemoryPointerBits);
    if (I->isLoad()) {
      const auto *Tuple = llvm::dyn_cast<llvm::StructType>(Call.getType());
      ResultValid = Tuple && !Tuple->isPacked() &&
                    Tuple->getNumElements() == Plan.Outputs;
      if (ResultValid) {
        ResultValid = Tuple->getElementType(I->vectors())
                          ->isIntegerTy(kAddressMemoryPointerBits);
        for (unsigned N = 0; N != I->vectors(); ++N)
          ResultValid &= Vector(Tuple->getElementType(N));
      }
    }
    if (TheArch != Arch::AArch64 || !ResultValid || Text != Plan.Text ||
        Asm->getConstraintString() != Plan.Constraints ||
        !Asm->hasSideEffects() || Asm->isAlignStack() || Asm->canThrow() ||
        Asm->getDialect() != llvm::InlineAsm::AD_ATT ||
        Call.arg_size() != Plan.Inputs || Call.getFunctionType()->isVarArg() ||
        Call.getNumOperandBundles() ||
        Call.getCallingConv() != llvm::CallingConv::C || Call.doesNotReturn() ||
        Call.hasFnAttr(llvm::Attribute::Speculatable) ||
        Call.getMemoryEffects() != llvm::MemoryEffects::unknown() ||
        !Call.getArgOperand(0)->getType()->isIntegerTy(
            kAddressMemoryPointerBits) ||
        !Call.getArgOperand(1)->getType()->isIntegerTy(
            kAddressMemoryPointerBits) ||
        (!I->stackBase() && I->base() != kAddressMemoryScratchBase) ||
        I->destination() != kStructureScratchVector ||
        (I->hasRegisterIndex() && I->index() != kStructureScratchIndex))
      throw std::invalid_argument(kWritebackMalformed);
    for (unsigned N = 2; N + 1 < Call.arg_size(); ++N)
      if (!Vector(Call.getArgOperand(N)->getType()))
        throw std::invalid_argument(kWritebackMalformed);
    if (!I->hasRegisterIndex()) {
      const auto *Index =
          llvm::dyn_cast<llvm::ConstantInt>(Call.getArgOperand(1));
      if (!Index || !Index->isZero())
        throw std::invalid_argument(kWritebackMalformed);
    }
    return AArch64WritebackMemoryInstruction{I->Word};
  }
  const auto *Machine =
      std::find_if(std::begin(AArch64WritebackMemoryMachines),
                   std::end(AArch64WritebackMemoryMachines),
                   [&](const auto &M) { return Text == M.Asm; });
  if (Machine == std::end(AArch64WritebackMemoryMachines)) {
    if (llvm::any_of(AArch64WritebackMemoryMachines, [&](const auto &M) {
          return llvm::StringRef(Text).starts_with(
              llvm::StringRef(M.Asm).split('\n').first);
        }))
      throw std::invalid_argument(kWritebackMalformed);
    return std::nullopt;
  }
  const bool Pair = Machine->Pair;
  const bool Vector = Machine->Vector;
  const bool Load = Machine->Load;
  const auto MatchesVector = [](llvm::Type *Type) {
    const auto *V = llvm::dyn_cast<llvm::FixedVectorType>(Type);
    return V &&
           V->getNumElements() ==
               kAddressMemoryVectorBytes / kAddressMemoryPointerBytes &&
           V->getElementType()->isIntegerTy(kAddressMemoryPointerBits);
  };
  const unsigned Count =
      Load ? kWritebackLoadOperands
           : (Pair ? kPairStoreOperands : kWritebackStoreOperands);
  const unsigned BaseIndex = Pair ? kPairBaseIndex : kWritebackBaseIndex;
  const auto *Result = llvm::dyn_cast<llvm::StructType>(Call.getType());
  bool ResultValid = Call.getType()->isIntegerTy(kAddressMemoryPointerBits);
  if (Load) {
    ResultValid = Result && !Result->isPacked() &&
                  Result->getNumElements() == BaseIndex + 1 &&
                  Result->getElementType(BaseIndex)->isIntegerTy(
                      kAddressMemoryPointerBits);
    if (ResultValid)
      for (unsigned I = 0; I != BaseIndex; ++I)
        ResultValid &= Vector ? MatchesVector(Result->getElementType(I))
                              : Result->getElementType(I)->isIntegerTy(
                                    kAddressMemoryPointerBits);
  }
  if (TheArch != Arch::AArch64 || !ResultValid ||
      Asm->getConstraintString() != Machine->Constraints ||
      !Asm->hasSideEffects() || Asm->isAlignStack() || Asm->canThrow() ||
      Asm->getDialect() != llvm::InlineAsm::AD_ATT ||
      Call.arg_size() != Count || Call.getFunctionType()->isVarArg() ||
      Call.getNumOperandBundles() ||
      Call.getCallingConv() != llvm::CallingConv::C || Call.doesNotReturn() ||
      Call.hasFnAttr(llvm::Attribute::Speculatable) ||
      Call.getMemoryEffects() != llvm::MemoryEffects::unknown() ||
      !Call.getArgOperand(0)->getType()->isIntegerTy(
          kAddressMemoryPointerBits) ||
      !Call.getArgOperand(Count - 1)->getType()->isIntegerTy(
          kAddressMemoryWordBytes * CHAR_BIT))
    throw std::invalid_argument(kWritebackMalformed);
  if (!Load)
    for (unsigned I = 1; I != Count - 1; ++I)
      if (Vector ? !MatchesVector(Call.getArgOperand(I)->getType())
                 : !Call.getArgOperand(I)->getType()->isIntegerTy(
                       kAddressMemoryPointerBits))
        throw std::invalid_argument(kWritebackMalformed);
  // LLVM store arguments are base, data, word; the source contract remains
  // base, word, data. The encoded word is metadata, never an address value.
  const auto *Word =
      llvm::dyn_cast<llvm::ConstantInt>(Call.getArgOperand(Count - 1));
  const auto Instruction =
      Word ? aarch64WritebackMemoryInstruction(Word->getZExtValue())
           : std::nullopt;
  if (!Instruction || Instruction->isLoad() != Load ||
      Instruction->isVector() != Vector || Instruction->isPair() != Pair ||
      (Pair && Instruction->secondDestination() != kPairSecondScratch) ||
      Instruction->stackBase() != Machine->StackBase ||
      (!Instruction->stackBase() &&
       Instruction->base() != kAddressMemoryScratchBase) ||
      Instruction->destination() != kAddressMemoryScratchValue)
    throw std::invalid_argument(kWritebackMalformed);
  return Instruction;
}
} // namespace

bool validateAArch64WritebackMemoryCall(Arch TheArch,
                                        const llvm::CallInst &Call) {
  return validatedAArch64WritebackMemoryCall(TheArch, Call).has_value();
}

std::optional<InlineAsmRender>
renderAArch64WritebackMemoryCall(Arch TheArch, const llvm::CallInst &Call,
                                 const std::string &ResultName, bool ResultLive,
                                 const std::vector<std::string> &Args) {
  const auto Instruction = validatedAArch64WritebackMemoryCall(TheArch, Call);
  if (!Instruction)
    return std::nullopt;
  if (Args.size() != Call.arg_size())
    throw std::invalid_argument(kWritebackMalformed);
  const bool Vector = Instruction->isVector();
  const bool Load = Instruction->isLoad();
  if (Instruction->isStructure()) {
    const auto I = Instruction->structure();
    std::vector<std::string> Data, Outputs;
    for (unsigned N = 2; N + 1 < Args.size(); ++N)
      Data.push_back(llvm::formatv(kAddressVectorToBitsC, Args[N]).str());
    if (Load)
      for (unsigned N = 0; N != I.vectors() + 1; ++N)
        Outputs.push_back(
            ResultLive && !ResultName.empty()
                ? llvm::formatv(kStructureCField, ResultName, N).str()
                : std::string());
    const auto Code =
        aarch64StructureMemoryC(I, Args[0], Args[1], Data, Outputs, true);
    if (Load)
      return InlineAsmRender{Code, false};
    return InlineAsmRender{
        llvm::formatv(ResultLive && !ResultName.empty()
                          ? kWritebackCAssignment
                          : kAArch64PointerCDiscardStatement,
                      ResultLive && !ResultName.empty() ? ResultName : Code,
                      Code)
            .str(),
        false};
  }
  if (Instruction->isPair()) {
    if (Load) {
      std::array<std::string, kPairBaseIndex + 1> Outputs;
      if (ResultLive && !ResultName.empty())
        for (unsigned I = 0; I != Outputs.size(); ++I)
          Outputs[I] = llvm::formatv(kPairCField, ResultName, I).str();
      return InlineAsmRender{
          aarch64PairMemoryCStatement(*Instruction, Args[0], Outputs, Vector),
          false};
    }
    const auto First =
        Vector ? llvm::formatv(kAddressVectorToBitsC, Args[1]).str() : Args[1];
    const auto Second =
        Vector ? llvm::formatv(kAddressVectorToBitsC, Args[2]).str() : Args[2];
    const auto Value =
        aarch64PairStoreMemoryC(*Instruction, Args[0], First, Second);
    return InlineAsmRender{
        llvm::formatv(ResultLive && !ResultName.empty()
                          ? kWritebackCAssignment
                          : kAArch64PointerCDiscardStatement,
                      ResultLive && !ResultName.empty() ? ResultName : Value,
                      Value)
            .str(),
        false};
  }
  if (Vector && Load)
    return InlineAsmRender{
        aarch64VectorWritebackMemoryCStatement(
            *Instruction, Args[0],
            ResultLive && !ResultName.empty()
                ? llvm::formatv(kWritebackCValueField, ResultName).str()
                : std::string(),
            ResultLive && !ResultName.empty()
                ? llvm::formatv(kWritebackCBaseField, ResultName).str()
                : std::string(),
            true),
        false};
  const auto Data = !Load && Vector
                        ? llvm::formatv(kAddressVectorToBitsC, Args[1]).str()
                        : (!Load ? Args[1] : std::string());
  const auto Value = aarch64WritebackMemoryC(*Instruction, Args[0], Data);
  if (!ResultLive || ResultName.empty())
    return InlineAsmRender{
        llvm::formatv(kAArch64PointerCDiscardStatement, Value).str(), false};
  if (!Load)
    return InlineAsmRender{
        llvm::formatv(kWritebackCAssignment, ResultName, Value).str(), false};
  const auto PairName =
      aarch64PAuthTemporary(kWritebackCPair, {ResultName, Args[0], Args[1]});
  return InlineAsmRender{
      llvm::formatv(kWritebackCStructAssignment, PairName, Value, ResultName)
          .str(),
      false};
}

std::optional<InlineAsmRender>
renderAArch64PAuthLoadCall(Arch TheArch, const llvm::CallInst &Call,
                           const std::string &ResultName, bool ResultLive,
                           const std::vector<std::string> &Args) {
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!Asm || (Asm->getAsmString() != kPAuthLoadAsm &&
               !Asm->getAsmString().starts_with(kPAuthLoadSPPrefix)))
    return std::nullopt;
  const bool StackBase = Asm->getAsmString() == kPAuthLoadSPAsm;
  const auto *Result = llvm::dyn_cast<llvm::StructType>(Call.getType());
  if (TheArch != Arch::AArch64 ||
      Asm->getAsmString() != (StackBase ? kPAuthLoadSPAsm : kPAuthLoadAsm) ||
      Asm->getConstraintString() !=
          (StackBase ? kPAuthLoadSPConstraints : kPAuthLoadConstraints) ||
      !Asm->hasSideEffects() || Asm->isAlignStack() || Asm->canThrow() ||
      Asm->getDialect() != llvm::InlineAsm::AD_ATT ||
      Call.getFunctionType()->isVarArg() || Call.getNumOperandBundles() ||
      Call.getCallingConv() != llvm::CallingConv::C || Call.doesNotReturn() ||
      Call.hasFnAttr(llvm::Attribute::Speculatable) ||
      Call.getMemoryEffects() != llvm::MemoryEffects::unknown() ||
      Call.arg_size() != kPAuthLoadOperandCount ||
      Args.size() != kPAuthLoadOperandCount || !Result || Result->isPacked() ||
      Result->getNumElements() != kPAuthLoadOperandCount ||
      !Result->getElementType(kPAuthLoadValueIndex)
           ->isIntegerTy(kPAuthLoadPointerBits) ||
      !Result->getElementType(kPAuthLoadBaseIndex)
           ->isIntegerTy(kPAuthLoadPointerBits) ||
      !Call.getArgOperand(0)->getType()->isIntegerTy(kPAuthLoadPointerBits) ||
      !Call.getArgOperand(1)->getType()->isIntegerTy(kPAuthLoadWordBytes *
                                                     CHAR_BIT))
    throw std::invalid_argument(kPAuthLoadMalformed);
  const auto *Word = llvm::dyn_cast<llvm::ConstantInt>(Call.getArgOperand(1));
  const auto Instruction =
      Word ? aarch64PAuthLoadInstruction(Word->getZExtValue()) : std::nullopt;
  if (!Instruction || Instruction->stackBase() != StackBase ||
      (!StackBase && Instruction->base() != kPAuthLoadScratchBase) ||
      Instruction->destination() != kPAuthLoadScratchValue)
    throw std::invalid_argument(kPAuthLoadMalformed);
  const auto Value = aarch64PAuthLoadC(*Instruction, Args[0]);
  if (!ResultLive || ResultName.empty())
    return InlineAsmRender{
        llvm::formatv(kAArch64PointerCDiscardStatement, Value).str(), false};
  const auto PairName =
      aarch64PAuthTemporary(kPAuthLoadCPair, {ResultName, Args[0], Args[1]});
  return InlineAsmRender{
      llvm::formatv(kPAuthLoadCStructAssignment, PairName, Value, ResultName)
          .str(),
      false};
}

const char *lookupArmAsmToC(const char *Mnem) {
  for (const auto &E : ARMAsmTable)
    if (std::strcmp(Mnem, E.AsmStr) == 0)
      return E.CName;
  return nullptr;
}

InlineAsmRender renderARMInlineAsm(Arch TheArch, const std::string &AsmStr,
                                   const std::string &Mnemonic,
                                   const std::string &ResultName,
                                   bool ResultLive,
                                   const std::vector<std::string> &Args) {
  const bool AArch64 = TheArch == Arch::AArch64;
  if (AArch64 && AsmStr == kAArch64AuthenticatedTargetUseAsm &&
      Args.size() == 1 && !ResultLive)
    return {aarch64PAuthTargetUseC(Args[0]) + "\n", false};
  // Preserve the tied input/output of machine PAC/AUT/XPAC; the generic asm
  // renderer cannot infer that constraint from operand numbers alone.
  if (AArch64) {
    for (const auto &Auth : llvm::concat<const AArch64PAuthLROperation>(
             kAArch64PAuthLROperations, kAArch64PAuthMOperations)) {
      if (AsmStr != Auth.llvmAsm())
        continue;
      const std::string Value = aarch64PAuthLRC(Auth, Args);
      return {
          ResultLive && !ResultName.empty()
              ? llvm::formatv(kAArch64PointerCAssignment, ResultName, Value)
                    .str()
              : llvm::formatv(kAArch64PointerCDiscardStatement, Value).str(),
          false};
    }
    for (const auto &Operation : kAArch64CPAOperations) {
      if (AsmStr != Operation.MachineAsm)
        continue;
      const auto Value = aarch64CPAC(Operation, Args);
      return {
          ResultLive && !ResultName.empty()
              ? llvm::formatv(kAArch64PointerCAssignment, ResultName, Value)
                    .str()
              : llvm::formatv(kAArch64PointerCDiscardStatement, Value).str(),
          false};
    }
    for (const auto &Auth : kAArch64PAuthOperations) {
      if (AsmStr != Auth.llvmAsm() || Args.size() != (Auth.Strip ? 1 : 2))
        continue;
      const std::string Value =
          aarch64PAuthMachineC(Auth, Args[0], Auth.Strip ? "" : Args[1]);
      return {
          ResultLive && !ResultName.empty()
              ? llvm::formatv(kAArch64PointerCAssignment, ResultName, Value)
                    .str()
              : llvm::formatv(kAArch64PointerCDiscardStatement, Value).str(),
          false};
    }
  }
  if (Mnemonic == "sel" && Args.size() >= 2) {
    std::string Result;
    if (ResultLive && !ResultName.empty())
      Result = ResultName + " = ";
    Result += "__sel(" + Args[0] + ", " + Args[1] + ");\n";
    return {Result, true};
  }

  // A barrier runs as the ACLE intrinsic of its own option: `dsb ishst` is
  // `__dsb(0xA)`, not the mnemonic's default.
  if (Mnemonic == "dmb" || Mnemonic == "dsb" || Mnemonic == "isb") {
    const llvm::StringRef Option =
        llvm::StringRef(AsmStr).drop_front(Mnemonic.size()).trim();
    const std::optional<uint64_t> CRm = Option.empty()
                                            ? std::optional<uint64_t>(0xF)
                                            : armBarrierOptionValue(Option);
    if (CRm && Args.empty())
      return {"__" + Mnemonic + "(0x" + llvm::utohexstr(*CRm) + ");" +
                  (Option.empty() ? "" : " /* " + AsmStr + " */") + "\n",
              true};
  }
  // An AArch64 system register as ACLE names it: `mrs $0, tpidr_el0` reads
  // __arm_rsr64("tpidr_el0"), and `msr tpidr_el0, $0` writes __arm_wsr64.
  // AArch32's status registers (`mrs $0, apsr`) are no such register.
  const auto isRegisterName = [](llvm::StringRef Name) {
    return !Name.empty() && llvm::all_of(Name, [](char C) {
      return llvm::isAlnum(C) || C == '_';
    });
  };
  if (llvm::StringRef Register = AsmStr;
      AArch64 && Register.consume_front("mrs $0, ") && Args.empty() &&
      isRegisterName(Register))
    return {(ResultLive && !ResultName.empty() ? ResultName + " = "
                                               : std::string()) +
                "__arm_rsr64(\"" + Register.str() + "\");\n",
            true};
  if (llvm::StringRef Register = AsmStr;
      AArch64 && Register.consume_front("msr ") &&
      Register.consume_back(", $0") && Args.size() == 1 &&
      isRegisterName(Register))
    return {"__arm_wsr64(\"" + Register.str() + "\", " + Args[0] + ");\n",
            true};

  // A coprocessor transfer is ACLE's __arm_mrc or kin, with the numbers its
  // assembly carries.
  if (const auto Transfer = armCoprocessorAsmField(AsmStr)) {
    const auto [Id, Field] = *Transfer;
    const bool Writes = Id == Intrinsic::ArmMcr || Id == Intrinsic::ArmMcrr;
    if (Args.size() == (Writes ? 1u : 0u))
      return {(!Writes && ResultLive && !ResultName.empty() ? ResultName + " = "
                                                            : std::string()) +
                  armCoprocessorAcleCall(Id, Field,
                                         Writes ? Args[0] : std::string()) +
                  ";\n",
              true};
  }

  const char *ArmC = lookupArmAsmToC(AsmStr.c_str());
  // Raw exception assembly preserves its code and continuation. Only an
  // explicit llvm.trap grants the stronger, non-returning builtin contract.
  std::string Comment;
  if (!ArmC) {
    ArmC = lookupArmAsmToC(Mnemonic.c_str());
    if (ArmC && AsmStr != Mnemonic)
      Comment = " /* " + AsmStr + " */";
  }
  if (ArmC)
    return {std::string(ArmC) + ";" + Comment + "\n", true};

  // LLVM's `$N` is GNU C's `%N`, and an operand the arguments do not fill is
  // the output, which comes first (`sysl $0, #3, c15, c0, #1`).
  std::string Template;
  size_t Operands = 0;
  for (size_t I = 0; I < AsmStr.size(); ++I) {
    if (AsmStr[I] == '$' && I + 1 < AsmStr.size() &&
        llvm::isDigit(AsmStr[I + 1])) {
      Operands = std::max<size_t>(Operands, AsmStr[I + 1] - '0' + 1);
      Template += '%';
      continue;
    }
    Template += AsmStr[I];
  }
  const bool HasOutput = Operands > Args.size();
  // An output nothing reads still needs a place to go, in a register.
  const bool ScratchOutput = HasOutput && (!ResultLive || ResultName.empty());
  std::string Result = ScratchOutput ? std::string("{ ") +
                                           (AArch64 ? "uint64_t" : "uint32_t") +
                                           " result; "
                                     : "";
  Result += "__asm__ volatile(\"" + Template + "\"";
  if (HasOutput || !Args.empty()) {
    Result += " : ";
    if (HasOutput)
      Result += "\"=r\"(" +
                (ScratchOutput ? std::string("result") : ResultName) + ") ";
    Result += ":";
    for (size_t I = 0; I < Args.size(); ++I)
      Result += (I > 0 ? ", " : " ") + ("\"r\"(" + Args[I] + ")");
    Result += " :";
  } else {
    Result += " :::";
  }
  // An AArch32 MSR writes the condition flags.
  Result +=
      !AArch64 && Mnemonic == "msr" ? " \"memory\", \"cc\");" : " \"memory\");";
  Result += ScratchOutput ? " }\n" : "\n";
  return {Result, false};
}

std::string renderARMFence(llvm::AtomicOrdering Ordering) {
  switch (Ordering) {
  case llvm::AtomicOrdering::SequentiallyConsistent:
    return "__dmb(0xF);\n";
  case llvm::AtomicOrdering::Acquire:
    return "__dmb(0x9);\n";
  case llvm::AtomicOrdering::Release:
    return "__dmb(0xA);\n";
  default:
    return "__asm__ volatile(\"dmb ish\" ::: \"memory\");\n";
  }
}

const char *renderARMDebugBreak() { return "__builtin_debugtrap();\n"; }

} // namespace neverd
