//===- X86AmxAsm.h - Native AMX architectural bank contract ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Preserve hardware TILECFG/TILEDATA and native restart without compiler
/// tile spills, synthetic configuration loads, or an undeclared runtime helper.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86AMXASM_H
#define NEVERD_BACKEND_LLVM_X86_X86AMXASM_H

#include "neverd/backend/llvm/X86/X86MemoryAddressAsm.h"
#include "neverd/ir/intrinsics/X86/X86AmxState.h"

#include <optional>
#include <string>
#include <vector>
namespace neverd {
inline constexpr const char *X86AmxAsmMarker = "# neverd AMX v2 ";
inline constexpr const char *X86AmxLegacyAsmMarker = "# neverd AMX v1 ";
inline constexpr const char *X86AmxStackLeafAsmMarker =
    "# neverd AMX stack leaf v1 ";
inline constexpr const char *X86AmxStackLeafAttribute =
    "neverd-x86-amx-stack-leaf";
struct X86AmxAsm {
  std::string Text;
  std::string Constraints;
  std::vector<std::string> InputConstraints;
  std::vector<uint16_t> InputBytes;
  std::vector<std::string> Clobbers;
  bool Row = false;
  bool Memory = false;
  bool StackLeaf = false;
};
inline std::optional<X86AmxAsm>
x86AmxAsm(uint32_t D, NdMemoryAddressSpace Space, bool Legacy = false) {
  if (!x86AmxDescriptorIsValid(D) || !x86AmxAddressIsValid(D, Space) ||
      (Legacy && (D & X86AmxVersionMask) != X86AmxLegacyStateVersion))
    return {};
  const auto K = x86AmxOperation(D);
  X86AmxAsm P;
  P.Row = x86AmxIsRow(K);
  P.Memory = x86AmxIsMemory(K);
  P.StackLeaf = x86AmxNeedsStackLeaf(D);
  P.Text = std::string(P.StackLeaf ? X86AmxStackLeafAsmMarker
                       : Legacy    ? X86AmxLegacyAsmMarker
                                   : X86AmxAsmMarker) +
           std::to_string(D) + " " +
           std::to_string(static_cast<unsigned>(Space)) + "\n\t";
  const bool Stack = D & X86AmxStackReference;
  if (Stack && !P.StackLeaf) {
    // Use a real stack-default base without replacing the compiler's RBP.
    // Arithmetic stays inside the owned window and performs no extra access.
    P.Text += "movq ${0:q}, %rdx\n\tsubq %rbp, %rdx\n\t";
    P.Clobbers = {"rdx", "flags"};
  }
  if (D & X86AmxEvexMemory)
    P.Text += "{evex} ";
  P.Text += X86AmxMnemonics[unsigned(K)];
  auto Tile = [&](unsigned I) {
    return "%tmm" + std::to_string(x86AmxTile(D, I));
  };
  if (P.Memory) {
    const auto Segment = Space == NdMemoryAddressSpace::X86FS   ? "%fs:"
                         : Space == NdMemoryAddressSpace::X86GS ? "%gs:"
                         : D & X86AmxStackSegment               ? "%ss:"
                                                                : "%ds:";
    const bool Addr32 = D & X86AmxAddress32;
    std::string Address = std::string(Segment) + (P.StackLeaf ? "(${0:q}"
                                                  : Stack     ? "(%rbp,%rdx"
                                                  : Addr32    ? "(${0:k}"
                                                              : "(${0:q}");
    P.InputBytes.push_back(Addr32 ? 4 : 8);
    P.InputConstraints.push_back(
        P.StackLeaf ? "{bp}"
        : Legacy    ? "r"
                    : x86MemoryAddressAsm(Addr32 ? 32 : 64).LLVMConstraint);
    if (x86AmxIsTileMemory(K)) {
      Address += Addr32 ? ",${1:k}" : ",${1:q}";
      P.InputBytes.push_back(Addr32 ? 4 : 8);
      P.InputConstraints.push_back(Legacy ? "r" : "{cx}");
    }
    Address += ')';
    if (K == X86AmxOp::Store)
      P.Text += " " + Tile(0) + ", " + Address;
    else if (x86AmxIsTileMemory(K))
      P.Text += " " + Address + ", " + Tile(0);
    else
      P.Text += " " + Address;
  } else if (P.Row) {
    P.InputBytes = {8, 4};
    P.InputConstraints = {"r", D & X86AmxImmediateRow  ? "i"
                               : D & X86AmxExtendedRow ? "{r16}"
                                                       : "r"};
    P.Text += std::string(" ") +
              (D & X86AmxImmediateRow ? "$$${1:c}" : "${1:k}") + ", " +
              Tile(0) + ", %zmm0\n\tvmovdqu64 %zmm0, ($0)";
    P.Clobbers.push_back("zmm0");
  } else if (K == X86AmxOp::TF32) {
    // Intel withdrew AMX-TF32 after ISE revision 059; LLVM 23 removed its
    // mnemonic. Retain the exact historical opcode, including native #UD on
    // CPUs without that extension, instead of substituting another compute.
    P.Text.erase(P.Text.size() -
                 std::string(X86AmxMnemonics[unsigned(K)]).size());
    P.Text += ".byte 0xc4, 0xe2, " +
              std::to_string(((~x86AmxTile(D, 2) & 15) << 3) | 1) + ", 0x48, " +
              std::to_string(0xc0 | (x86AmxTile(D) << 3) | x86AmxTile(D, 1)) +
              " # tmmultf32ps " + Tile(2) + ", " + Tile(1) + ", " + Tile(0);
  } else if (x86AmxIsCompute(K))
    P.Text += " " + Tile(2) + ", " + Tile(1) + ", " + Tile(0);
  else if (K == X86AmxOp::Zero)
    P.Text += " " + Tile(0);
  // The bank is architectural state, not LLVM tile-valued SSA. Declare the
  // complete bank unavailable to host allocation across every operation.
  for (unsigned I = 0; I != 8; ++I)
    P.Clobbers.push_back("tmm" + std::to_string(I));
  P.Clobbers.push_back("memory");
  auto Add = [&](const std::string &S) {
    if (!P.Constraints.empty())
      P.Constraints += ',';
    P.Constraints += S;
  };
  for (const auto &C : P.InputConstraints)
    Add(C);
  for (const auto &C : P.Clobbers)
    Add("~{" + C + "}");
  return P;
}
} // namespace neverd
#endif // NEVERD_BACKEND_LLVM_X86_X86AMXASM_H
