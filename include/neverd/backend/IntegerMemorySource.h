//===- IntegerMemorySource.h - Native ordering and reservation assembly -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Keep exact instruction widths, fence domains and acquire/release bits when
/// rebuilding RISC-V and PowerPC source.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_INTEGERMEMORYSOURCE_H
#define NEVERD_BACKEND_INTEGERMEMORYSOURCE_H
#include "neverd/ir/intrinsics/IntegerMemory.h"

#include "llvm/ADT/ArrayRef.h"

#include <string>

namespace neverd::integer_memory {
inline constexpr char SourceMarker[] = "neverd.integer.memory";

inline std::string assembly(Arch A, Form F, unsigned Key, char Operand) {
  std::string Name = F.Name;
  const std::string Out = std::string(1, Operand) + "0",
                    Address = std::string(1, Operand) + "1",
                    Value = std::string(1, Operand) + "2";
  if (F.Op == Operation::Fence) {
    if (!isRISCV(A))
      return Name;
    // Encode even the empty predecessor/successor sets (architectural hints).
    if (!(Key & 15) || !(Key >> 4))
      return ".4byte " + std::to_string((Key << 20) | 15);
    auto Domain = [](unsigned Mask) {
      std::string S;
      for (unsigned I = 0; I < 4; ++I)
        if (Mask & (8 >> I))
          S += "iorw"[I];
      return S;
    };
    return "fence " + Domain(Key >> 4) + ", " + Domain(Key & 15);
  }
  if (isRISCV(A)) {
    constexpr const char *Orders[] = {"", ".rl", ".aq", ".aqrl"};
    Name += F.Bytes == 4 ? ".w" : ".d";
    Name += Orders[F.Ordering];
    return Name + " " + Out + ", " + (F.inputs() == 2 ? Value + ", " : "") +
           "(" + Address + ")";
  }
  if (F.Op == Operation::Reserve)
    return Name + " " + Out + ", 0, " + Address;
  // Report the EQ bit written by the conditional store, before compiler
  // code can overwrite CR0. The remaining modeled CR0 bits have fixed sources.
  return Name + " " + Value + ", 0, " + Address + "; mfcr " + Out +
         "; rlwinm " + Out + ", " + Out + ", 3, 31, 31";
}

inline std::string constraints(Arch A, Form F) {
  if (!F.inputs())
    return "~{memory}";
  std::string C = F.inputs() == 1 ? "=r,r" : "=r,r,r";
  if (isPowerPC(A) && F.Op == Operation::Conditional)
    C += ",~{cr0}";
  return C + ",~{memory}";
}

inline std::string source(Arch A, Form F, unsigned Key,
                          llvm::ArrayRef<std::string> Args) {
  std::string Asm = "__asm__ volatile (\"" + assembly(A, F, Key, '%') + "\"";
  if (!F.inputs())
    return "(__extension__ ({ " + Asm + " ::: \"memory\"); }))";
  Asm += " : \"=r\" (__nd_result) : ";
  for (size_t I = 0; I < Args.size(); ++I) {
    if (I)
      Asm += ", ";
    Asm += "\"r\" ((__UINTPTR_TYPE__)(" + Args[I] + "))";
  }
  Asm += " : \"memory\"";
  if (isPowerPC(A) && F.Op == Operation::Conditional)
    Asm += ", \"cr0\"";
  return "(__extension__ ({ __UINTPTR_TYPE__ __nd_result; " + Asm +
         "); __nd_result; }))";
}
} // namespace neverd::integer_memory
#endif // NEVERD_BACKEND_INTEGERMEMORYSOURCE_H
