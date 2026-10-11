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
inline constexpr char X86FPApprox12ValueConstraints[] = "=&x,x,~{memory}";
inline constexpr char X86FPApprox12MemoryConstraints[] = "=&x,r,~{memory}";

inline llvm::Type *x86FPArithStateLLVMType(llvm::LLVMContext &Context,
                                           unsigned Layout) {
  const unsigned Control = x86FPArithStateControl(Layout);
  const unsigned Element = x86FPArithStateElementBytes(Control);
  auto *Scalar = Element == 4 ? llvm::Type::getFloatTy(Context)
                              : llvm::Type::getDoubleTy(Context);
  return x86FPArithStateIsScalar(Control)
             ? Scalar
             : llvm::FixedVectorType::get(
                   Scalar, x86FPStateSourceBytes(Layout) / Element);
}

inline const char *x86FPArithStateConstraints(unsigned Layout, bool Memory) {
  const unsigned Control = x86FPArithStateControl(Layout);
  if ((Control & 7) == unsigned(X86FPArithKind::FusedMultiplyAdd))
    return Memory ? "=&x,0,x,r,r,~{memory}" : "=&x,0,x,x,r,~{memory}";
  const bool Unary = x86FPArithStateIsUnary(Control);
  return Unary ? (Memory ? X86FPStateRoundMemoryConstraints
                         : X86FPStateRoundConstraints)
               : (Memory ? "=&x,0,r,r,~{memory}" : X86FPStateBinaryConstraints);
}

inline std::string x86FPArithStateAsm(unsigned Layout, bool Memory) {
  const unsigned Control = x86FPArithStateControl(Layout);
  const bool Unary = x86FPArithStateIsUnary(Control);
  const bool Vex = x86FPStateSourceBytes(Layout) == 32 || (Control & 32);
  const auto Space = x86FPRoundStateAddressSpace(Layout);
  const char *Segment = Space == NdMemoryAddressSpace::X86FS   ? "%fs:"
                        : Space == NdMemoryAddressSpace::X86GS ? "%gs:"
                                                               : "";
  if ((Control & 7) == unsigned(X86FPArithKind::FusedMultiplyAdd))
    return "ldmxcsr ($4)\n\t" + x86FPArithStateMnemonic(Layout) + " " +
           (Memory ? std::string(Segment) + "($3)" : "$3") +
           ",$2,$0\n\tstmxcsr ($4)";
  const std::string RHS = "$" + std::to_string(Unary ? 1 : 2);
  const std::string State = "$" + std::to_string(Unary ? 2 : 3);
  return "ldmxcsr (" + State + ")\n\t" + x86FPArithStateMnemonic(Layout) + " " +
         (Memory ? std::string(Segment) + "(" + RHS + ")" : RHS) +
         (!Unary && Vex ? ",$1" : "") + ",$0\n\tstmxcsr (" + State + ")";
}

inline llvm::Type *x86FPApprox12LLVMType(llvm::LLVMContext &Context,
                                         unsigned Layout) {
  auto *Scalar = llvm::Type::getFloatTy(Context);
  return x86FPApprox12IsScalar(x86FPRoundStateControl(Layout))
             ? Scalar
             : llvm::FixedVectorType::get(Scalar,
                                          x86FPStateSourceBytes(Layout) / 4);
}

inline std::string x86FPApprox12Asm(unsigned Layout, bool Memory) {
  const auto Space = x86FPRoundStateAddressSpace(Layout);
  const char *Segment = Space == NdMemoryAddressSpace::X86FS   ? "%fs:"
                        : Space == NdMemoryAddressSpace::X86GS ? "%gs:"
                                                               : "";
  return std::string(x86FPApprox12Mnemonic(Layout)) + " " +
         (Memory ? std::string(Segment) + "($1)" : "$1") + ",$0";
}

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
  if (Call.arg_size() == 1)
    for (bool Memory : {false, true})
      for (unsigned Control = 0; Control < (Memory ? 8U : 4U); ++Control)
        for (unsigned Bytes : {4U, 16U, 32U}) {
          if (x86FPApprox12IsScalar(Control)
                  ? Bytes != 4 || (Control & 4)
                  : Bytes == 4 || (Memory && Bytes == 32 && !(Control & 4)))
            continue;
          for (auto Space :
               {NdMemoryAddressSpace::Default, NdMemoryAddressSpace::X86FS,
                NdMemoryAddressSpace::X86GS}) {
            if (!Memory && Space != NdMemoryAddressSpace::Default)
              continue;
            const unsigned Layout =
                x86FPRoundStateLayout(Bytes, Control, 0, Space);
            const auto *Type = x86FPApprox12LLVMType(Call.getContext(), Layout);
            const auto *SourceType = Call.getArgOperand(0)->getType();
            if (Call.getType() == Type &&
                (Memory ? SourceType->isPointerTy() &&
                              SourceType->getPointerAddressSpace() ==
                                  llvmX86MemoryAddressSpace(Space)
                        : SourceType == Type) &&
                Asm->getConstraintString() ==
                    (Memory ? X86FPApprox12MemoryConstraints
                            : X86FPApprox12ValueConstraints) &&
                Asm->getAsmString() == x86FPApprox12Asm(Layout, Memory))
              return std::pair{Memory ? Intrinsic::X86FPApprox12MemoryState
                                      : Intrinsic::X86FPApprox12State,
                               Layout};
          }
        }
  for (bool Memory : {false, true})
    for (unsigned Order = 0; Order < 3; ++Order)
      for (bool Double : {false, true})
        for (bool Scalar : {false, true})
          for (unsigned Operation = 0; Operation < 6; ++Operation)
            for (unsigned Bytes : {4U, 8U, 16U, 32U}) {
              if (Scalar ? Bytes != (Double ? 8U : 4U) || Operation >= 4
                         : Bytes < 16)
                continue;
              const unsigned Control =
                  makeX86FPFmaStateControl(Order, Double, Scalar, Memory,
                                           Operation < 4 && (Operation & 2),
                                           Operation < 4 && (Operation & 1),
                                           Operation >= 4, Operation == 4);
              for (auto Space :
                   {NdMemoryAddressSpace::Default, NdMemoryAddressSpace::X86FS,
                    NdMemoryAddressSpace::X86GS}) {
                if (!Memory && Space != NdMemoryAddressSpace::Default)
                  continue;
                const unsigned Layout =
                    x86FPRoundStateLayout(Bytes, Control, 0, Space);
                const auto *Type =
                    x86FPArithStateLLVMType(Call.getContext(), Layout);
                if (Call.arg_size() == 4 && Call.getType() == Type &&
                    Call.getArgOperand(0)->getType() == Type &&
                    Call.getArgOperand(1)->getType() == Type &&
                    (Memory ? Call.getArgOperand(2)->getType()->isPointerTy() &&
                                  Call.getArgOperand(2)
                                          ->getType()
                                          ->getPointerAddressSpace() ==
                                      llvmX86MemoryAddressSpace(Space)
                            : Call.getArgOperand(2)->getType() == Type) &&
                    PointerIsDefault(Call.getArgOperand(3)) &&
                    Asm->getConstraintString() ==
                        x86FPArithStateConstraints(Layout, Memory) &&
                    Asm->getAsmString() == x86FPArithStateAsm(Layout, Memory))
                  return std::pair{Memory ? Intrinsic::X86FPFmaMemoryState
                                          : Intrinsic::X86FPFmaState,
                                   Layout};
              }
            }
  for (bool Memory : {false, true})
    for (unsigned Control = 0; Control < 256; ++Control)
      for (unsigned Bytes : {4U, 8U, 16U, 32U}) {
        const bool Unary = x86FPArithStateIsUnary(Control);
        const bool Scalar = x86FPArithStateIsScalar(Control);
        if (!x86FPArithStateControlIsValid(Control, Memory) ||
            !x86FPArithStateOperation(Control) ||
            (Scalar
                 ? Bytes != x86FPArithStateElementBytes(Control) ||
                       (Control & 32)
                 : Bytes < 16 || (Memory && Bytes == 32 && !(Control & 32))) ||
            Call.arg_size() != (Unary ? 2U : 3U))
          continue;
        for (auto Space :
             {NdMemoryAddressSpace::Default, NdMemoryAddressSpace::X86FS,
              NdMemoryAddressSpace::X86GS}) {
          if (!Memory && Space != NdMemoryAddressSpace::Default)
            continue;
          const unsigned Layout =
              x86FPRoundStateLayout(Bytes, Control, 0, Space);
          const auto *Type = x86FPArithStateLLVMType(Call.getContext(), Layout);
          const auto *RHS = Call.getArgOperand(Unary ? 0 : 1)->getType();
          if (Call.getType() == Type &&
              (Unary || Call.getArgOperand(0)->getType() == Type) &&
              (Memory
                   ? RHS->isPointerTy() && RHS->getPointerAddressSpace() ==
                                               llvmX86MemoryAddressSpace(Space)
                   : RHS == Type) &&
              PointerIsDefault(Call.getArgOperand(Unary ? 1 : 2)) &&
              Asm->getConstraintString() ==
                  x86FPArithStateConstraints(Layout, Memory) &&
              Asm->getAsmString() == x86FPArithStateAsm(Layout, Memory))
            return std::pair{Memory ? Intrinsic::X86FPArithMemoryState
                                    : Intrinsic::X86FPArithState,
                             Layout};
        }
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
