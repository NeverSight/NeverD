//===- MIPSR6FPSource.h - Release 6 native scalar FP -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// Execute the original COP1 instruction with its ambient FCSR, retaining
/// rounding, NaNs, sticky flags and enabled exceptions through C and LLVM.
#ifndef NEVERD_BACKEND_MIPS_MIPSR6FPSOURCE_H
#define NEVERD_BACKEND_MIPS_MIPSR6FPSOURCE_H
#include "neverd/backend/MIPS/MIPSFPAssembly.h"
#include "neverd/ir/intrinsics/MIPS/MIPSR6FPOperations.h"

#include <stdexcept>

namespace neverd::mips {
inline FPNativeAssembly r6FPAssembly(Intrinsic Id, unsigned Selector,
                                     bool Micro) {
  FPNativeAssembly A;
  A.Text = ".set push\n.set fp=64\n";
  A.Read = Id == Intrinsic::MIPSReadFPControl;
  A.Write = Id == Intrinsic::MIPSWriteFPControl;
  if (A.Read || A.Write) {
    if (!r6FPControlSelector(Selector, A.Write))
      throw std::invalid_argument("invalid R6 FP control selector");
    A.Constraints = A.Read ? "=r,~{memory}" : "r,~{memory}";
    A.Text += std::string(A.Read ? "cfc1 %0,$" : "ctc1 %0,$") +
              std::to_string(Selector);
  } else {
    const auto &F = r6FPOperation(Id);
    A.Pointers = F.Arity + 1;
    A.Constraints = "r";
    for (unsigned I = 0; I < F.Arity; ++I) {
      A.Text += std::string(F.InputBytes == 8 ? "ldc1 " : "lwc1 ") + "$f" +
                std::to_string(2 * I) + ",0(%" + std::to_string(I + 1) + ")\n";
      A.Constraints += ",r";
    }
    const unsigned FS = F.Arity == 3 ? 2 : 0, FT = F.Arity == 3   ? 4
                                                   : F.Arity == 2 ? 2
                                                                  : 0;
    auto Word = F.encoding() | (FS << 11) | (FT << 16);
    if (Micro) {
      const auto Key = F.encoding();
      uint32_t Base = 0;
      switch (Key) {
#define NEVERD_MICRO_FP_BINARY(Minor, Classic)                                 \
  case Classic:                                                                \
    Base = Minor | (FT << 21) | (FS << 16);                                    \
    break;
#define NEVERD_MICRO_FP_UNARY(Minor, Classic)                                  \
  case Classic:                                                                \
    Base = Minor | (FS << 16);                                                 \
    break;
#define NEVERD_MICRO_FP_REVERSED(Minor, Classic)                               \
  case Classic:                                                                \
    Base = Minor | (FS << 21);                                                 \
    break;
#include "neverd/lift/MIPS/MicroMIPSFloat.def"
      default:
        if ((Key >> 21 & 31) != 20 && (Key >> 21 & 31) != 21)
          throw std::invalid_argument("missing microMIPS FP encoding");
        Base = ((Key >> 21 & 1) ? 21 : 5) | ((Key & 31) << 6) | (FT << 21) |
               (FS << 16);
      }
      Word = 0x54000000 | Base;
      A.Text += ".2byte " + std::to_string(Word >> 16) + ", " +
                std::to_string(Word & 65535);
    } else
      A.Text += ".word " + std::to_string(Word);
    A.Text +=
        std::string(F.OutputBytes == 8 ? "\nsdc1 " : "\nswc1 ") + "$f0,0(%0)";
    A.Constraints += ",~{memory}";
    for (unsigned I = 0; I < F.Arity; ++I)
      A.Constraints += ",~{$f" + std::to_string(2 * I) + "}";
  }
  A.Text += "\n.set pop";
  return A;
}
} // namespace neverd::mips
#endif // NEVERD_BACKEND_MIPS_MIPSR6FPSOURCE_H
