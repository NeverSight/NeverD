//===- MIPSMSASource.h - Native MSA execution -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The original MSA instruction owns rounding, exception masks, sticky flags,
/// non-signaling results, NaN propagation and implementation-specific
/// estimates. Transfer numeric 128-bit SSA values through private aligned stack
/// slots.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_MIPS_MIPSMSASOURCE_H
#define NEVERD_BACKEND_MIPS_MIPSMSASOURCE_H

#include "neverd/ir/intrinsics/MIPS/MIPSScalar.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

namespace neverd::mips {

struct MSANativeAssembly {
  std::string Text;
  std::string Constraints;
  unsigned Pointers = 0;
  bool ClobbersAT = false;
  bool Read = false;
  bool Write = false;
  std::string llvmText() const {
    std::string S;
    for (char C : Text) {
      if (C == '$')
        S += '$';
      S += C == '%' ? '$' : C;
    }
    return S;
  }
  std::string cStatement(llvm::ArrayRef<std::string> Arguments,
                         llvm::StringRef Result = {}) const {
    std::string S;
    llvm::raw_string_ostream OS(S);
    OS << "__asm__ volatile(\"";
    OS.write_escaped(Text);
    OS << "\" : ";
    if (Read)
      OS << "\"=r\"(" << Result << ")";
    OS << " : ";
    for (unsigned I = 0; I < Arguments.size(); ++I) {
      if (I)
        OS << ", ";
      OS << "\"r\"(" << Arguments[I] << ")";
    }
    OS << " : \"memory\"";
    if (ClobbersAT)
      OS << ", \"$1\"";
    for (unsigned I = 1; I < Pointers; ++I)
      OS << ", \"$f" << 2 * (I - 1) << "\"";
    OS << ");";
    return S;
  }
};

inline MSANativeAssembly msaAssembly(Intrinsic Id, unsigned Selector,
                                     bool Little) {
  MSANativeAssembly A;
  A.Text = ".set push\n.set msa\n";
  A.Read = Id == Intrinsic::MIPSReadMSAControl;
  A.Write = Id == Intrinsic::MIPSWriteMSAControl;
  if (A.Read || A.Write) {
    A.Constraints = A.Read ? "=r,~{memory}" : "r,~{memory}";
    if (Selector < 8) {
      const auto Control = "$" + std::to_string(Selector);
      A.Text += A.Read ? "cfcmsa %0, " + Control : "ctcmsa " + Control + ", %0";
    } else {
      // LLVM's assembler only names the eight implemented control registers.
      // Preserve the reserved selector in the instruction that actually runs.
      A.ClobbersAT = true;
      A.Constraints += ",~{$1}";
      const uint32_t Word = A.Read ? 0x787e0059u | (Selector << 11)
                                   : 0x783e0819u | (Selector << 6);
      A.Text += ".set noat\n";
      if (!A.Read)
        A.Text += "move $1,%0\n";
      A.Text += ".word " + std::to_string(Word);
      if (A.Read)
        A.Text += "\nmove %0,$1";
    }
  } else {
    A.ClobbersAT = true;
    const auto &Op = msaFloatOperation(Id);
    A.Pointers = 1 + Op.Arity;
    // Renaming FP operands to scratch WRs is valid only without hardware
    // register partitioning. Stop before any WR access on a WRP machine.
    A.Text +=
        ".set noat\ncfcmsa $1, $0\nsrl $1,$1,16\nandi $1,$1,1\ntne $1,$0\n";
    std::string Operands = " $w0";
    A.Constraints = "r";
    for (unsigned I = 0; I < Op.Arity; ++I) {
      const auto Reg = "$w" + std::to_string(2 * I);
      A.Text += "ld.w " + Reg + ", 0(%" + std::to_string(I + 1) + ")\n";
      if (!Little)
        A.Text += "shf.w " + Reg + ", " + Reg + ", 27\n";
      if (Op.Arity != 3 || I)
        Operands += ", " + Reg;
      A.Constraints += ",r";
    }
    A.Text += Op.Mnemonic + Operands + "\n";
    if (!Little)
      A.Text += "shf.w $w0, $w0, 27\n";
    A.Text += "st.w $w0, 0(%0)";
    A.Constraints += ",~{memory},~{$1}";
    for (unsigned I = 0; I < Op.Arity; ++I)
      A.Constraints += ",~{$f" + std::to_string(2 * I) + "}";
  }
  A.Text += "\n.set pop";
  return A;
}

} // namespace neverd::mips

#endif // NEVERD_BACKEND_MIPS_MIPSMSASOURCE_H
