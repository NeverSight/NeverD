//===- X86RepStateAsm.h - Owned REP completion assembly -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One fallback template owns the DF window and full register completion.
/// The local branch preserves MPX bounds. No stack scratch or memory probe
/// is inserted around the actual REP access.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86REPSTATEASM_H
#define NEVERD_BACKEND_LLVM_X86_X86REPSTATEASM_H

#include "neverd/ir/intrinsics/X86/X86RepState.h"

#include <optional>
#include <string>
#include <vector>

namespace neverd {

inline constexpr char X86RepAsmMarker[] = "# neverd-rep-state-v1:";

struct X86RepAsm {
  Intrinsic Id;
  uint32_t Descriptor;
  uint16_t WordBytes;
  X86RepControl Control;
  X86RepContract Contract;
  std::string Text;
  std::string Constraints;
  std::vector<std::string> OutputConstraints;
  std::vector<std::string> InputConstraints;
};

inline std::optional<X86RepAsm> x86RepAsm(Intrinsic Id, Arch A,
                                          uint32_t Descriptor) {
  const auto C = x86RepContract(Id);
  const auto Control = x86RepControl(Id, A, Descriptor);
  if (!C || !Control)
    return std::nullopt;
  X86RepAsm P{
      Id,       Descriptor, static_cast<uint16_t>(A == Arch::X64 ? 8 : 4),
      *Control, *C,         {},
      {},       {},         {}};
  if (Id == Intrinsic::X86RepMovsState || Id == Intrinsic::X86RepCmpsState) {
    P.OutputConstraints = {"={si}", "={di}", "={cx}"};
    P.InputConstraints = {"0", "1", "2", "r"};
  } else if (Id == Intrinsic::X86RepLodsState) {
    P.OutputConstraints = {"={si}", "={cx}", "={ax}"};
    P.InputConstraints = {"0", "1", "2", "r"};
  } else {
    P.OutputConstraints = {C->Source ? "={si}" : "={di}", "={cx}"};
    P.InputConstraints = {"0", "1", C->Port ? "{dx}" : "{ax}", "r"};
  }
  if (C->Compare) {
    P.OutputConstraints.push_back("={ax}");
    if (Id == Intrinsic::X86RepScasState) {
      P.InputConstraints[2] = "2";
      P.OutputConstraints.push_back("={dx}");
    }
    P.InputConstraints.push_back("3");
  }
  const auto Df = std::to_string(P.OutputConstraints.size() + 3);
  P.Text = X86RepAsmMarker + std::string(C->Name) + ":" +
           std::to_string(Descriptor) + "\n\tcld\n\ttestl $" + Df + ",$" + Df +
           "\n\trepne; je 1f\n\tstd\n\t1:\n\t";
  if (C->Compare) {
    if (Id == Intrinsic::X86RepScasState)
      P.Text += A == Arch::X64 ? "xchgq %rax,%rdx\n\t" : "xchgl %eax,%edx\n\t";
    P.Text += "andl $$" + std::to_string(X86RepFlagsMask) +
              ",%eax\n\taddb $$127,%al\n\tsahf\n\t";
    if (Id == Intrinsic::X86RepScasState)
      P.Text += A == Arch::X64 ? "movq %rdx,%rax\n\t" : "movl %edx,%eax\n\t";
  }
  if (Control->Segment)
    P.Text += ".byte " + std::to_string(Control->Segment) + "\n\t";
  if (Control->AddressBytes != P.WordBytes)
    P.Text += Control->AddressBytes == 4 ? "addr32 " : "addr16 ";
  P.Text += Control->NotEqual ? "repne " : "rep ";
  const char Suffix = Control->ElementBytes == 8   ? 'q'
                      : Control->ElementBytes == 4 ? 'l'
                      : Control->ElementBytes == 2 ? 'w'
                                                   : 'b';
  P.Text += std::string(C->Name) + Suffix;
  if (C->Compare)
    P.Text += "\n\tlahf\n\tseto %al\n\tandl $$" +
              std::to_string(X86RepFlagsMask) + ",%eax";
  P.Text += "\n\tcld";
  for (const auto &Constraint : P.OutputConstraints)
    P.Constraints += Constraint + ',';
  for (const auto &Constraint : P.InputConstraints)
    P.Constraints += Constraint + ',';
  P.Constraints += "~{memory},~{dirflag},~{flags}";
  return P;
}

inline std::string x86RepNativeName(Intrinsic Id, Arch A) {
  const auto C = x86RepContract(Id);
  if (!C || (A != Arch::X86 && A != Arch::X64))
    return {};
  return "llvm.x86.rep." + std::string(C->Name) + ".state" +
         (A == Arch::X86 ? "32" : "");
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_X86REPSTATEASM_H
