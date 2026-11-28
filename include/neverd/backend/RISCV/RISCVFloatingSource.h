//===- RISCVFloatingSource.h - Native RISC-V FP operations ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_RISCV_RISCVFLOATINGSOURCE_H
#define NEVERD_BACKEND_RISCV_RISCVFLOATINGSOURCE_H

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/intrinsics/RISCV/RISCVFloating.h"

#include "llvm/ADT/ArrayRef.h"

#include <string>

namespace neverd::riscv {
struct FPAssembly {
  FPValue Result;
  std::array<FPValue, 3> Inputs;
  unsigned Count;
  std::string Text, Constraints;
};

inline std::optional<FPAssembly> fpAssembly(Intrinsic Id, uint64_t Selector,
                                            unsigned XLen, char Operand) {
  const auto Arg = [Operand](unsigned I) {
    return std::string(1, Operand) + std::to_string(I);
  };
  if (Id == Intrinsic::RISCVFPControl) {
    const auto CSR = fpControl(Selector);
    if (!CSR)
      return std::nullopt;
    std::string Text =
        std::string(CSR->mnemonic()) + " " + Arg(0) + ", " + CSR->name();
    if (CSR->Operation)
      Text += ", " + Arg(1);
    return FPAssembly{FPValue::X,
                      {FPValue::X},
                      unsigned(CSR->Operation != 0),
                      Text,
                      CSR->Operation ? "=r,r" : "=r"};
  }
  const auto *F = floatingForm(Selector, XLen);
  if (Id != Intrinsic::RISCVFloating || !F)
    return std::nullopt;
  std::string Text = std::string(F->Mnemonic) + " " + Arg(0);
  std::string Constraints = fpRegister(F->Result) ? "=f" : "=r";
  for (unsigned I = 0; I < F->arity(); ++I) {
    Text += ", " + Arg(I + 1);
    Constraints += fpRegister(F->Inputs[I]) ? ",f" : ",r";
  }
  // GNU and LLVM assemblers omit rm from the exact widening conversions.
  // Preserve its actual field, including dynamic mode, in those encodings.
  const bool ExactDouble =
      F->Result == FPValue::D && (F->Operation == FPOperation::Convert ||
                                  F->Operation == FPOperation::FromW ||
                                  F->Operation == FPOperation::FromWU);
  if (ExactDouble) {
    const bool Convert = F->Operation == FPOperation::Convert;
    Text = ".insn r 0x53, " + std::to_string(Selector & 7) + ", " +
           (Convert ? "0x21" : "0x69") + ", " + Arg(0) + ", " + Arg(1) +
           (F->Operation == FPOperation::FromWU ? ", x1" : ", x0");
  } else if (F->Rounded)
    Text += ", " + std::string(FPRoundingNames[Selector & 7]);
  return FPAssembly{F->Result, F->Inputs, F->arity(), Text, Constraints};
}
inline const char *fpIntegerC(unsigned Bytes) {
  return Bytes == 8 ? "__UINT64_TYPE__" : "__UINT32_TYPE__";
}
inline const char *fpNativeC(FPValue V, unsigned XLen) {
  if (V == FPValue::S)
    return "float";
  if (V == FPValue::D)
    return "double";
  return fpIntegerC(fpBytes(V, XLen));
}
/// Arguments and result here have the native constraint types. HighC adapts
/// its raw machine carriers at the boundary; LLVM C already has typed values.
inline std::string fpSource(const FPAssembly &Asm,
                            llvm::ArrayRef<std::string> Args, unsigned XLen) {
  std::string S = std::string("(__extension__ ({ ") +
                  fpNativeC(Asm.Result, XLen) +
                  " __nd_result; __asm__ volatile (\"" + Asm.Text + "\" : \"" +
                  (fpRegister(Asm.Result) ? "=f" : "=r") + "\" (__nd_result)";
  for (unsigned I = 0; I < Args.size(); ++I)
    S += std::string(I ? ", " : " : ") +
         (fpRegister(Asm.Inputs[I]) ? "\"f\"" : "\"r\"") + " (" + Args[I] + ")";
  return S + "); __nd_result; }))";
}
} // namespace neverd::riscv
#endif
