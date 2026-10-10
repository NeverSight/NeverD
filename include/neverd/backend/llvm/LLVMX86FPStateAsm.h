//===- LLVMX86FPStateAsm.h - Scalar FP completion assembly -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_LLVMX86FPSTATEASM_H
#define NEVERD_BACKEND_LLVM_LLVMX86FPSTATEASM_H

#include "neverd/backend/llvm/LLVMX86AddressSpaces.h"
#include "neverd/ir/X86FPState.h"

#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/ErrorHandling.h"

#include <optional>
#include <string>

namespace neverd {

inline constexpr char X86FPStateAsmMetadata[] = "neverd.x86.fp-state";
inline constexpr char X86FPStateReadAsm[] = "stmxcsr ($0)";
inline constexpr char X86FPStateWriteAsm[] = "ldmxcsr ($0)";
inline constexpr char X86FPStateTransferConstraints[] = "r,~{memory}";
inline constexpr char X86FPStateBinaryConstraints[] = "=&x,0,x,r,~{memory}";
inline constexpr char X86FPStateConversionConstraints[] = "=&r,x,r,~{memory}";
inline constexpr char X86FPStateRoundConstraints[] = "=&x,x,r,~{memory}";
inline constexpr char X86FPStateRoundMemoryConstraints[] = "=&x,r,r,~{memory}";

inline llvm::Type *x86FPRoundStateLLVMType(llvm::LLVMContext &Context,
                                           unsigned Layout) {
  const unsigned Control = x86FPRoundStateControl(Layout);
  const unsigned Element = x86FPRoundStateElementBytes(Control);
  auto *Scalar = Element == 4 ? llvm::Type::getFloatTy(Context)
                              : llvm::Type::getDoubleTy(Context);
  return x86FPRoundStateIsScalar(Control)
             ? Scalar
             : llvm::FixedVectorType::get(
                   Scalar, x86FPStateSourceBytes(Layout) / Element);
}

inline std::string x86FPRoundStateAsm(unsigned Layout) {
  return "ldmxcsr ($2)\n\t" + std::string(x86FPRoundStateMnemonic(Layout)) +
         " $$" + std::to_string(x86FPRoundStateImmediate(Layout)) +
         ",$1,$0\n\tstmxcsr ($2)";
}

inline std::string x86FPRoundMemoryStateAsm(unsigned Layout) {
  const auto Space = x86FPRoundStateAddressSpace(Layout);
  const char *Segment = Space == NdMemoryAddressSpace::X86FS   ? "%fs:"
                        : Space == NdMemoryAddressSpace::X86GS ? "%gs:"
                                                               : "";
  return "ldmxcsr ($2)\n\t" + std::string(x86FPRoundStateMnemonic(Layout)) +
         " $$" + std::to_string(x86FPRoundStateImmediate(Layout)) + "," +
         Segment + "($1),$0\n\tstmxcsr ($2)";
}

inline std::string x86FPStateConversionAsm(Intrinsic Id, unsigned SourceBytes) {
  return "ldmxcsr ($2)\n\t" +
         std::string(x86FPStateConversionMnemonic(Id, SourceBytes)) +
         " $1,$0\n\tstmxcsr ($2)";
}

inline std::string x86FPStateBinaryAsm(Intrinsic Id, unsigned ScalarSize) {
  return "ldmxcsr ($3)\n\t" + std::string(x86ScalarFPStateMnemonic(Id)) +
         (ScalarSize == 4 ? "ss" : "sd") + " $2,$0\n\tstmxcsr ($3)";
}

/// Only owned, fully typed assembly contracts may acquire FP state source
/// helpers. A mnemonic alone cannot prove the operand roles or completion.
inline std::optional<std::pair<Intrinsic, unsigned>>
classifyX86FPStateAsm(const llvm::CallInst &Call) {
  if (!Call.getMetadata(X86FPStateAsmMetadata))
    return std::nullopt;
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!Asm || !Asm->hasSideEffects() || Asm->isAlignStack() ||
      Asm->canThrow() || Asm->getDialect() != llvm::InlineAsm::AD_ATT)
    llvm::report_fatal_error("invalid x86 FP state assembly properties");
  const auto PointerIsDefault = [](const llvm::Value *Value) {
    return Value->getType()->isPointerTy() &&
           Value->getType()->getPointerAddressSpace() == 0;
  };
  if (Call.getType()->isVoidTy() && Call.arg_size() == 1 &&
      PointerIsDefault(Call.getArgOperand(0)) &&
      Asm->getConstraintString() == X86FPStateTransferConstraints) {
    if (Asm->getAsmString() == X86FPStateReadAsm)
      return std::pair{Intrinsic::X86ReadMXCSR, 0U};
    if (Asm->getAsmString() == X86FPStateWriteAsm)
      return std::pair{Intrinsic::X86WriteMXCSR, 0U};
  }
  for (Intrinsic Id : {Intrinsic::X86FPAddState, Intrinsic::X86FPSubState,
                       Intrinsic::X86FPMulState, Intrinsic::X86FPDivState})
    for (unsigned Bytes : {4U, 8U}) {
      const bool TypeMatches = Bytes == 4 ? Call.getType()->isFloatTy()
                                          : Call.getType()->isDoubleTy();
      if (TypeMatches && Call.arg_size() == 3 &&
          Call.getArgOperand(0)->getType() == Call.getType() &&
          Call.getArgOperand(1)->getType() == Call.getType() &&
          PointerIsDefault(Call.getArgOperand(2)) &&
          Asm->getConstraintString() == X86FPStateBinaryConstraints &&
          Asm->getAsmString() == x86FPStateBinaryAsm(Id, Bytes))
        return std::pair{Id, Bytes};
    }
  if (Call.arg_size() == 2 && PointerIsDefault(Call.getArgOperand(1)) &&
      Asm->getConstraintString() == X86FPStateRoundConstraints)
    for (unsigned Control : {0U, 2U, 4U, 6U})
      for (unsigned Bytes : {4U, 8U, 16U, 32U}) {
        if (x86FPRoundStateIsScalar(Control)
                ? Bytes != x86FPRoundStateElementBytes(Control)
                : Bytes < 16)
          continue;
        for (unsigned Immediate = 0; Immediate < 16; ++Immediate) {
          const unsigned Layout =
              x86FPRoundStateLayout(Bytes, Control, Immediate);
          if (Call.getType() ==
                  x86FPRoundStateLLVMType(Call.getContext(), Layout) &&
              Call.getArgOperand(0)->getType() == Call.getType() &&
              Asm->getAsmString() == x86FPRoundStateAsm(Layout))
            return std::pair{Intrinsic::X86FPRoundState, Layout};
        }
      }
  for (Intrinsic Id :
       {Intrinsic::X86FPCvtToIntState, Intrinsic::X86FPTruncToIntState})
    for (unsigned SourceBytes : {4U, 8U})
      for (unsigned DestinationBytes : {4U, 8U}) {
        if (!Call.getType()->isIntegerTy(DestinationBytes * 8) ||
            Call.arg_size() != 2 ||
            !(SourceBytes == 4
                  ? Call.getArgOperand(0)->getType()->isFloatTy()
                  : Call.getArgOperand(0)->getType()->isDoubleTy()) ||
            !PointerIsDefault(Call.getArgOperand(1)))
          continue;
        if (Asm->getConstraintString() == X86FPStateConversionConstraints &&
            Asm->getAsmString() == x86FPStateConversionAsm(Id, SourceBytes))
          return std::pair{
              Id, x86FPConversionLayout(SourceBytes, DestinationBytes)};
      }
  if (Call.arg_size() == 2 && Call.getArgOperand(0)->getType()->isPointerTy() &&
      PointerIsDefault(Call.getArgOperand(1)) &&
      Asm->getConstraintString() == X86FPStateRoundMemoryConstraints)
    for (unsigned Control : {0U, 2U, 4U, 6U, 8U, 10U})
      for (unsigned Bytes : {4U, 8U, 16U, 32U}) {
        if (x86FPRoundStateIsScalar(Control)
                ? Bytes != x86FPRoundStateElementBytes(Control)
                : Bytes != 16 && (!(Control & 8) || Bytes != 32))
          continue;
        for (auto Space :
             {NdMemoryAddressSpace::Default, NdMemoryAddressSpace::X86FS,
              NdMemoryAddressSpace::X86GS}) {
          if (Call.getArgOperand(0)->getType()->getPointerAddressSpace() !=
              llvmX86MemoryAddressSpace(Space))
            continue;
          for (unsigned Immediate = 0; Immediate < 16; ++Immediate) {
            const unsigned Layout =
                x86FPRoundStateLayout(Bytes, Control, Immediate, Space);
            if (Call.getType() ==
                    x86FPRoundStateLLVMType(Call.getContext(), Layout) &&
                Asm->getAsmString() == x86FPRoundMemoryStateAsm(Layout))
              return std::pair{Intrinsic::X86FPRoundMemoryState, Layout};
          }
        }
      }
  llvm::report_fatal_error("invalid x86 FP state assembly contract");
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_LLVMX86FPSTATEASM_H
