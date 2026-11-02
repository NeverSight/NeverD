//===- LLVMX87GuardedValueAsm.h - Guarded native x87 values ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate typed native value projections with explicit control and
/// operand guards. Unsupported inputs trap before the native operation.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_LLVMX87GUARDEDVALUEASM_H
#define NEVERD_BACKEND_LLVM_X86_LLVMX87GUARDEDVALUEASM_H

#include "neverd/backend/llvm/X86/LLVMX86X87StateAsm.h"
#include "neverd/ir/X86/X87Control.h"
#include "neverd/ir/X86/X87GuardedValue.h"

#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/ErrorHandling.h"

#include <string>

namespace neverd {

inline constexpr char X87GuardedValueAsmMetadata[] = "neverd.x87.guarded-value";

inline std::string x87GuardedValueConstraints() {
  return "r,r,r,r,~{eax}," + x87StateConstraints(X87StateEffect::Reset).str();
}

inline std::string x87GuardedValueAsm(Intrinsic Id) {
  if (!isX87GuardedValueIntrinsic(Id))
    llvm::report_fatal_error("invalid guarded x87 value operation");
  std::string Text =
      "movzwl ($2), %eax\n\ttestl $$" + std::to_string(X87ControlReservedMask) +
      ", %eax\n\tjnz 1f\n\tandl $$" + std::to_string(X87PrecisionControlMask) +
      ", %eax\n\tcmpl $$" + std::to_string(X87ReservedPrecisionControl) +
      ", %eax\n\tje 1f\n\tmovzwl ($2), %eax\n\tandl $$" +
      std::to_string(X87ExceptionMaskBits) + ", %eax\n\tcmpl $$" +
      std::to_string(X87ExceptionMaskBits) + ", %eax\n\tjne 1f\n\t";
  if (Id == Intrinsic::X87LoadPackedBCD)
    for (unsigned Byte = 0; Byte != 9; ++Byte) {
      const auto Offset = std::to_string(Byte);
      Text += "movzbl " + Offset +
              "($1), %eax\n\tandl $$15, %eax\n\tcmpl $$9, %eax\n\tja 1f\n\t"
              "movzbl " +
              Offset +
              "($1), %eax\n\tshrl $$4, %eax\n\tcmpl $$9, %eax\n\tja 1f\n\t";
    }
  if (isX87PushPairIntrinsic(Id)) {
    const unsigned Limit =
        Id == Intrinsic::X87FxtractPair ? 0x7fff : X87TrigRangeExponent;
    // Canonical finite operands only. The integer bit is clear exactly when
    // the exponent is zero; special and noncanonical encodings are excluded.
    Text += "movzwl 8($1), %eax\n\tandl $$32767, %eax\n\tcmpl $$" +
            std::to_string(Limit) +
            ", %eax\n\tjae 1f\n\ttestl %eax, %eax\n\tjz 3f\n\t"
            "testb $$128, 7($1)\n\tjz 1f\n\tjmp 4f\n3:\n\t"
            "testb $$128, 7($1)\n\tjnz 1f\n4:\n\t";
    Text += "fldcw ($2)\n\tfldt ($1)\n\t";
    Text += x87PushPairMnemonic(Id);
    Text += "\n\tfnstsw ($3)\n\tfstpt ($0)\n\tfstpt 10($0)";
    return Text + "\n\tjmp 2f\n1:\n\tud2\n2:";
  }
  Text += "fldcw ($2)\n\t";
  // Capture the instruction's status before the temporary FBLD result is
  // popped. Compiler-generated spills must not become its status producer.
  Text += Id == Intrinsic::X87LoadPackedBCD
              ? "fbld ($1)\n\tfnstsw ($3)\n\tfstpt ($0)"
              : "fldt ($1)\n\tfbstp ($0)\n\tfnstsw ($3)";
  return Text + "\n\tjmp 2f\n1:\n\tud2\n2:";
}

inline std::optional<Intrinsic>
classifyX87GuardedValueAsm(const llvm::CallInst &Call) {
  if (!Call.getMetadata(X87GuardedValueAsmMetadata))
    return std::nullopt;
  const auto Invalid = []() -> std::optional<Intrinsic> {
    llvm::report_fatal_error("invalid guarded x87 value assembly");
  };
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!Asm || !Asm->hasSideEffects() || Asm->isAlignStack() ||
      Asm->getDialect() != llvm::InlineAsm::AD_ATT ||
      !Call.getType()->isVoidTy() || Call.getFunctionType()->isVarArg() ||
      Call.arg_size() != 4 ||
      Asm->getConstraintString() != x87GuardedValueConstraints())
    return Invalid();
  for (const auto &Arg : Call.args())
    if (!Arg->getType()->isPointerTy() ||
        Arg->getType()->getPointerAddressSpace() != 0)
      return Invalid();
  for (Intrinsic Id : {Intrinsic::X87LoadPackedBCD,
                       Intrinsic::X87StorePackedBCD, Intrinsic::X87FptanPair,
                       Intrinsic::X87FsincosPair, Intrinsic::X87FxtractPair})
    if (Asm->getAsmString() == x87GuardedValueAsm(Id))
      return Id;
  return Invalid();
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_LLVMX87GUARDEDVALUEASM_H
